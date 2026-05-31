# Strategy: Modern Audio Driver Implementation

## Objective

Replace legacy audio backends (ALSA-only, broken macOS/Windows) with modern, feature-rich drivers for Linux, macOS, and Windows 11, supporting professional audio workflows.

## Status & relationship to proposal 04 (as-built)

Parts of this strategy have shipped (see `plan/STATE.md`): the audio abstraction
layer and the WASAPI backend are implemented, ALSA was extracted behind the
interface, and the as-built `AudioBackend` evolved into a **callback-pull**
model (`setRenderCallback` + `getConfig` + `name`) rather than the
push/`writeAudio` sketch below. Proposal 04 then made **data format a property
of every wire** (`twFormat`), added a `twResampler` inside `twSpeaker`, and a
`twNegotiator` that resolves one rate per wire over a candidate domain `D`.

This revision wires the *backend* into that picture. Today a backend only
*returns* the rate it happened to open at, and the speaker resamples to it
unconditionally. To fully support the wire format a backend must also:

1. **return the rates it can open natively** (`supportedRates()`), so the
   negotiator can extend `D` with them and *prefer* a rate that needs no
   resampling; and
2. **accept a requested rate** (`openDevice(device, preferredRate)`), so when the
   project/graph rate is one the device supports natively, the host opens there
   and the speaker resampler collapses to a passthrough.

The section "Wire-format rate negotiation" below is the normative interface for
the rate/format aspect and supersedes the speculative `AudioConfig`/`AudioBackend`
sketch under "Audio Abstraction Layer" wherever they differ.

## High-Level Design

### Audio Abstraction Layer

All platform backends implement a common interface:

```cpp
namespace audio {

struct AudioConfig {
    uint32_t sampleRate = 44100;
    uint32_t channels = 2;
    uint32_t bufferFrames = 256;
    AudioFormat format = AudioFormat::Float32;  // or S16, F64
};

struct AudioDevice {
    std::string id;
    std::string name;
    uint32_t maxInputChannels = 0;
    uint32_t maxOutputChannels = 2;
    uint32_t defaultSampleRate = 44100;
};

class AudioBackend {
public:
    virtual ~AudioBackend() = default;
    
    // Device management
    virtual std::vector<AudioDevice> enumerateDevices() const = 0;
    virtual std::string getDefaultDevice() const = 0;
    
    // Lifecycle
    virtual int openDevice(const std::string& deviceId) = 0;
    virtual int closeDevice() = 0;
    virtual int startOutput() = 0;
    virtual int stopOutput() = 0;
    virtual bool isRunning() const = 0;
    
    // Audio I/O (callback-driven or polling)
    virtual int writeAudio(const float* samples, uint32_t frames) = 0;
    
    // Configuration
    virtual int setConfig(const AudioConfig& config) = 0;
    virtual AudioConfig getConfig() const = 0;
    virtual uint32_t getLatency() const = 0;  // ms
    
    // Error handling
    virtual std::string getLastError() const = 0;
};

std::unique_ptr<AudioBackend> createAudioBackend(AudioBackendType type);

}  // namespace audio
```

## Wire-format rate negotiation (normative)

This is the part that makes a backend a first-class participant in proposal 04's
format negotiation. It extends the **as-built** callback-pull interface; it does
not reintroduce the push model above.

### Interface additions

```cpp
namespace audio {

struct AudioConfig {
    std::uint32_t sampleRate   = 44100;
    std::uint32_t channels     = 2;
    std::uint32_t bufferFrames = 1024;
    std::uint32_t periodFrames = 64;
    twSampleType  sampleType   = twSampleType::Float32;  // NEW: device-native
                                                         // binary format
};

class AudioBackend {
public:
    // ... as-built: openDevice/closeDevice/startOutput/stopOutput/isRunning,
    //     setRenderCallback, getConfig, name ...

    // NEW — the rates this device can be opened at WITHOUT the host resampling.
    // A shared-mode backend reports its single mix rate; an exclusive-capable
    // backend may report several (44100/48000/96000/...). Empty == "unknown
    // until opened" (caller falls back to getConfig() after open). Pure query;
    // may probe the device, must not disturb an active stream.
    virtual std::vector<std::uint32_t> supportedRates() const = 0;

    // NEW — open while *requesting* a preferred rate. The backend opens at
    // preferredRate iff it can support it natively, otherwise at its default /
    // mix rate. 0 == "no preference; use the device default". The rate (and
    // sampleType) actually opened is reported by getConfig().
    virtual int openDevice(const std::string &device = "default",
                           std::uint32_t preferredRate = 0) = 0;
};

}  // namespace audio
```

`getConfig()` is the "return" half (the rate/format actually in force);
`supportedRates()` + the `preferredRate` argument are the "request" half. Both
speak in the same units as `twFormat.sampleRate` / `twFormat.sampleType`, so the
device boundary is just another wire as far as the negotiator is concerned.

### Negotiation flow (how it ties into twNegotiator / twSpeaker)

1. **Extend the candidate domain.** Before negotiating, the speaker seeds
   `D = env.candidateRates() ∪ {projectRate} ∪ backend.supportedRates()`. This
   is the resolution of proposal 04's open item *"whether to auto-extend D with
   rates a device advertises"* — yes, via `supportedRates()`.
2. **Resolve with a no-resample preference.** `twNegotiator` resolves the graph
   to a single rate, preferring (in order) a rate that is **both** the project
   rate **and** device-supported, then any device-supported rate, then the
   project rate. The goal is to land on a rate the device can open natively.
3. **Request it.** `twSpeaker::startOutput` calls
   `openDevice("default", negotiatedRate)`.
4. **Reconcile the remainder.** It reads back `getConfig()`; the
   `twResampler` is configured `graphRate → getConfig().sampleRate` (a
   passthrough when the request was honored) and the converter targets
   `getConfig().sampleType` directly (no float→int round-trip — already done via
   `twConvertFrames`).

Net effect: on a device that can open at the project rate (always true in
exclusive mode, and true in shared mode when the mix rate already matches), there
is **zero resampling**. Otherwise the speaker resampler bridges the gap exactly
as it does today — the path is never worse than the current behaviour.

### Shared vs. exclusive mode

`preferredRate` is **best-effort and mode-dependent**:

- **Shared mode** (default, polite): the OS mixer owns the rate. `supportedRates()`
  returns just the current mix rate and `preferredRate` is effectively advisory —
  if it differs from the mix rate the host resamples. No other app is disturbed.
- **Exclusive mode** (opt-in, "bit-perfect"): the device can be opened at any
  rate it advertises, so `preferredRate` is honored natively and `supportedRates()`
  enumerates the real hardware set. Trade-off: it seizes the endpoint. This is
  the mechanism behind proposal 04's deferred *"anchor priority (device vs.
  fixed-rate source)"* fork — exclusive mode lets a fixed-rate source drive the
  device rate.

A backend that implements only shared mode still satisfies the interface:
`supportedRates()` returns `{mixRate}` and `preferredRate` is honored only when
it equals the mix rate.

## Platform-Specific Implementations

### 1. Linux Audio: Multi-Backend Strategy

#### 1.1 ALSA (Advanced Linux Sound Architecture)

**Status**: Keep as primary, modernize existing code

**Current Implementation**:
- `tw303a/src/twspeaker.cc` lines 244-303 (device open)
- Async PCM handler (lines 418-433)
- Hardcoded 44.1 kHz, S16_LE, stereo

**Modernization**:

```cpp
// tw303a/src/audio/alsa_backend.cc
class ALSABackend : public AudioBackend {
private:
    snd_pcm_t* pcmHandle_ = nullptr;
    snd_async_handler_t* asyncHandler_ = nullptr;
    AudioConfig config_;
    std::string lastError_;
    
    static void asyncCallbackStatic_(snd_async_handler_t* handler);
    void asyncCallback_();
    int configureHardware_(const AudioConfig& config);
    
public:
    std::vector<AudioDevice> enumerateDevices() const override;
    int openDevice(const std::string& deviceId) override;
    int closeDevice() override;
    int startOutput() override;
    int stopOutput() override;
    int writeAudio(const float* samples, uint32_t frames) override;
    // ... rest of interface
};
```

**Improvements**:
- Device enumeration via `snd_card_next()` / `snd_ctl_*()` APIs
- Dynamic sample rate / format negotiation
- Error recovery (xrun handling)
- Configurable latency (buffer/period sizing)
- Better error messages

**Native-rate support (wire format)**:
- `supportedRates()`: probe with `snd_pcm_hw_params_test_rate(...)` over the
  candidate set (or read min/max via `snd_pcm_hw_params_get_rate_min/max`). ALSA
  hw devices are commonly flexible, so several rates usually pass.
- `openDevice(device, preferredRate)`: request it with
  `snd_pcm_hw_params_set_rate_near(...)`; ALSA picks the nearest supported rate
  and `getConfig().sampleRate` reports what was actually set.
- `getConfig().sampleType` stays `Int16` (S16_LE) as the device format; the
  shared `twConvertFrames` already produces it from the float graph buffer.

**Dependencies**:
- `libasound2` (already present)
- `libasound2-dev` (headers)

**Priority**: HIGH — Already working, needs modernization

---

#### 1.2 PipeWire (Modern Sound Server)

**Status**: NEW, optional for Linux developers

**Why PipeWire?**
- Replacing PulseAudio in modern distros (Fedora 38+, Ubuntu 24.04+)
- Lower latency, better for professional audio
- Unified approach: replaces PulseAudio + JACK + ALSA routing
- AppArmor/SELinux compatible
- Already supports ALSA underneath

**Implementation**:

```cpp
// tw303a/src/audio/pipewire_backend.cc
class PipeWireBackend : public AudioBackend {
private:
    pw_stream* stream_ = nullptr;
    pw_core* core_ = nullptr;
    pw_context* context_ = nullptr;
    spa_hook core_listener_;
    spa_hook stream_listener_;
    
    static void onStreamProcess_(void* userData);
    void processAudio_(struct pw_buffer* buffer);
    
public:
    std::vector<AudioDevice> enumerateDevices() const override;
    int openDevice(const std::string& deviceId) override;
    // ... interface impl
};
```

**API Overview**:
```cpp
// 1. Create context & core
pw_init(nullptr, nullptr);
pw_context* context = pw_context_new(nullptr, nullptr, 0);
pw_core* core = pw_context_connect(context, nullptr, 0);

// 2. Enumerate devices
pw_core_sync(core, PW_ID_CORE, 0);  // Sync to get device info

// 3. Create stream
pw_stream* stream = pw_stream_new_simple(
    core, "smaragd", 
    pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_AUDIO_CHANNELS, "2",
        nullptr),
    &stream_events, this);

// 4. Connect & run
pw_stream_connect(stream, PW_DIRECTION_OUTPUT, 0, nullptr);
```

**Build Integration**:
```cmake
# CMakeLists.txt
if(ENABLE_PIPEWIRE)
    pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)
    target_link_libraries(tw303a PRIVATE ${PIPEWIRE_LIBRARIES})
    target_include_directories(tw303a PRIVATE ${PIPEWIRE_INCLUDE_DIRS})
    target_sources(tw303a PRIVATE src/audio/pipewire_backend.cc)
endif()
```

**Dependencies**:
- `libpipewire-0.3-dev` (Debian/Ubuntu)
- `pipewire-devel` (Fedora/RHEL)

**Priority**: MEDIUM — Future-proof, opt-in

---

#### 1.3 JACK (Jack Audio Connection Kit)

**Status**: NEW, professional audio support

**Why JACK?**
- De-facto standard for professional audio on Linux
- Enables routing to other JACK apps (DAWs, effects, etc.)
- Low latency, sample-accurate sync
- Smaragd synthesizer as a JACK client = DAW integration

**Implementation** (simplified):

```cpp
// tw303a/src/audio/jack_backend.cc
class JACKBackend : public AudioBackend {
private:
    jack_client_t* client_ = nullptr;
    jack_port_t* outPortL_ = nullptr;
    jack_port_t* outPortR_ = nullptr;
    
    static int processCallback_(jack_nframes_t frames, void* arg);
    int process_(jack_nframes_t frames);
    
public:
    int openDevice(const std::string& serverName = "default") override;
    int startOutput() override;
    // ... interface impl
};

// Callback implementation
int JACKBackend::processCallback_(jack_nframes_t frames, void* arg) {
    JACKBackend* self = static_cast<JACKBackend*>(arg);
    return self->process_(frames);
}

int JACKBackend::process_(jack_nframes_t frames) {
    // Get output buffers
    float* bufL = (float*)jack_port_get_buffer(outPortL_, frames);
    float* bufR = (float*)jack_port_get_buffer(outPortR_, frames);
    
    // Fill from synthesizer
    twSpeaker::renderFrames(bufL, bufR, frames);
    return 0;
}
```

**Build Integration**:
```cmake
if(ENABLE_JACK)
    pkg_check_modules(JACK REQUIRED jack)
    target_link_libraries(tw303a PRIVATE ${JACK_LIBRARIES})
    target_sources(tw303a PRIVATE src/audio/jack_backend.cc)
endif()
```

**Dependencies**:
- `libjack-dev` (Debian/Ubuntu)
- `jack-audio-connection-kit-devel` (Fedora)

**Priority**: MEDIUM-LOW — Professional audio, opt-in

---

### 2. macOS Audio: CoreAudio Modernization

**Current State**: Broken (OS X 10.2 API, pre-2005)

**Broken APIs to Replace**:
- `OpenAComponent()` → `AudioComponentInstanceNew()`
- `FindNextComponent()` → `AudioComponentFindNext()`

**Target**: macOS 10.13+ (High Sierra, ~2017+)

#### Option A: Modern CoreAudio (Low-level, Full Control)

```cpp
// tw303a/src/audio/coreaudio_backend.cc
class CoreAudioBackend : public AudioBackend {
private:
    AudioUnit outputUnit_ = nullptr;
    AUGraph graph_ = nullptr;
    AudioBufferList* bufferList_ = nullptr;
    
    static OSStatus renderCallback_(
        void* refCon,
        AudioUnitRenderActionFlags* ioActionFlags,
        const AudioTimeStamp* inTimeStamp,
        UInt32 inBusNumber,
        UInt32 inNumberFrames,
        AudioBufferList* ioData);
    
    OSStatus render_(UInt32 frames, AudioBufferList* ioData);
    int setupAudioGraph_();
    
public:
    int openDevice(const std::string& deviceId) override;
    int startOutput() override;
    // ... interface
};

// Implementation
OSStatus CoreAudioBackend::renderCallback_(
    void* refCon, AudioUnitRenderActionFlags* ioActionFlags,
    const AudioTimeStamp* inTimeStamp, UInt32 inBusNumber,
    UInt32 inNumberFrames, AudioBufferList* ioData) {
    
    CoreAudioBackend* self = (CoreAudioBackend*)refCon;
    return self->render_(inNumberFrames, ioData);
}

int CoreAudioBackend::openDevice(const std::string& deviceId) {
    OSStatus err = 0;
    
    // 1. Create AudioComponentDescription for default output
    AudioComponentDescription outputDesc = {
        kAudioUnitType_Output,
        kAudioUnitSubType_DefaultOutput,
        kAudioUnitManufacturer_Apple,
        0, 0
    };
    
    // 2. Find component (modern API)
    AudioComponent outputComponent = 
        AudioComponentFindNext(nullptr, &outputDesc);
    if (!outputComponent) {
        lastError_ = "Output AudioComponent not found";
        return -1;
    }
    
    // 3. Create instance (replaces OpenAComponent)
    err = AudioComponentInstanceNew(outputComponent, &outputUnit_);
    if (err) {
        lastError_ = "Failed to create AudioComponentInstance";
        return -1;
    }
    
    // 4. Set render callback
    AURenderCallbackStruct renderCallback = {
        renderCallback_, this
    };
    err = AudioUnitSetProperty(outputUnit_,
        kAudioUnitProperty_SetRenderCallback,
        kAudioUnitScope_Global, 0,
        &renderCallback, sizeof(renderCallback));
    if (err) {
        lastError_ = "Failed to set render callback";
        return -1;
    }
    
    // 5. Configure format
    AudioStreamBasicDescription asbd = {
        .mSampleRate = 44100,
        .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked,
        .mBytesPerPacket = 8,
        .mFramesPerPacket = 1,
        .mBytesPerFrame = 8,
        .mChannelsPerFrame = 2,
        .mBitsPerChannel = 32,
    };
    
    err = AudioUnitSetProperty(outputUnit_,
        kAudioUnitProperty_StreamFormat,
        kAudioUnitScope_Input, 0,
        &asbd, sizeof(asbd));
    if (err) {
        lastError_ = "Failed to set stream format";
        return -1;
    }
    
    return 0;
}
```

**Advantages**:
- Direct CoreAudio control
- Minimal overhead
- Full latency optimization
- Works on all modern macOS versions

**Disadvantages**:
- More boilerplate
- Requires understanding AudioUnit architecture

#### Option B: AVAudioEngine (High-level, Simpler)

**Available in macOS 10.13+**

```swift
// Bridging layer (mixed Obj-C++ approach)
import AVFoundation

class SwiftAudioEngine {
    let engine = AVAudioEngine()
    let mixer = AVAudioMixerNode()
    
    func setup() {
        engine.attach(mixer)
        engine.connect(mixer, to: engine.outputNode,
                      format: nil)
        
        do {
            try engine.start()
        } catch {
            NSLog("Audio engine error: %@", error.localizedDescription)
        }
    }
}
```

**Advantages**:
- Simpler API
- Automatic hardware optimization
- Better macOS integration

**Disadvantages**:
- Less control
- Bridging Swift/C++ more complex
- Newer API (requires 10.13+)

**Recommendation**: Go with **Option A (Modern CoreAudio)** for compatibility, control, and consistency with existing C++ codebase.

**Build Integration**:
```cmake
if(APPLE AND ENABLE_COREAUDIO)
    find_library(COREAUDIO_LIB CoreAudio)
    find_library(AUDIOUNIT_LIB AudioUnit)
    find_library(AUDIOTOOLBOX_LIB AudioToolbox)
    
    target_link_libraries(tw303a PRIVATE
        ${COREAUDIO_LIB} ${AUDIOUNIT_LIB} ${AUDIOTOOLBOX_LIB})
    
    target_sources(tw303a PRIVATE src/audio/coreaudio_backend.cc)
    target_compile_definitions(tw303a PRIVATE QBX_MACOS_COREAUDIO=1)
endif()
```

**Native-rate support (wire format)**:
- `supportedRates()`: read the device's advertised rates via
  `kAudioDevicePropertyAvailableNominalSampleRates`.
- `openDevice(device, preferredRate)`: set
  `kAudioDevicePropertyNominalSampleRate` to `preferredRate` (CoreAudio can
  retune the device), then configure the AudioUnit's stream format to match;
  `getConfig().sampleRate` reports the resulting nominal rate.
- `getConfig().sampleType` is `Float32` (the canonical AudioUnit format), so the
  device boundary needs no type conversion at all.

**Dependencies**: Built-in (macOS SDK)

**Priority**: HIGH — Critical for macOS support

---

### 3. Windows Audio: WASAPI Implementation

**Current State**: Not implemented

**Why WASAPI (Windows Audio Session API)?**
- Standard Windows audio API since Vista (2007)
- Supports Windows 11 natively
- Low latency, exclusive mode available
- Device enumeration, format negotiation built-in
- Works with various audio devices (USB, Bluetooth, virtual)

**Implementation**:

```cpp
// tw303a/src/audio/wasapi_backend.cc
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

class WASAPIBackend : public AudioBackend {
private:
    ComPtr<IMMDeviceEnumerator> enumerator_;
    ComPtr<IMMDevice> device_;
    ComPtr<IAudioClient> audioClient_;
    ComPtr<IAudioRenderClient> renderClient_;
    HANDLE bufferReadyEvent_ = nullptr;
    std::thread audioThread_;
    bool running_ = false;
    
    HRESULT configureAudioClient_(const AudioConfig& config);
    void audioThreadProc_();
    int fillBuffer_();
    
public:
    std::vector<AudioDevice> enumerateDevices() const override;
    int openDevice(const std::string& deviceId) override;
    int startOutput() override;
    int stopOutput() override;
    // ... interface
};

// Implementation
int WASAPIBackend::openDevice(const std::string& deviceId) {
    HRESULT hr = S_OK;
    
    // 1. Create device enumerator
    ComPtr<IMMDeviceEnumerator> enumerator;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                         CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) {
        lastError_ = "Failed to create device enumerator";
        return -1;
    }
    
    // 2. Get render device (speakers)
    ComPtr<IMMDevice> device;
    if (deviceId == "default") {
        hr = enumerator->GetDefaultAudioEndpoint(
            eRender, eConsole, &device);
    } else {
        // deviceId is GUID from enumeration
        LPWSTR wDeviceId = ...; // Convert deviceId
        hr = enumerator->GetDevice(wDeviceId, &device);
    }
    if (FAILED(hr)) {
        lastError_ = "Failed to get audio device";
        return -1;
    }
    
    // 3. Activate audio client
    ComPtr<IAudioClient> audioClient;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER,
                         nullptr, (void**)&audioClient);
    if (FAILED(hr)) {
        lastError_ = "Failed to activate audio client";
        return -1;
    }
    
    // 4. Get device format (what hardware wants)
    WAVEFORMATEX* waveFormat = nullptr;
    hr = audioClient->GetMixFormat(&waveFormat);
    if (FAILED(hr)) {
        lastError_ = "Failed to get mix format";
        return -1;
    }
    
    // 5. Initialize audio client (shared mode, 10ms buffer)
    REFERENCE_TIME bufferDuration = 10 * 10000;  // 10ms in 100ns units
    hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                 bufferDuration, 0,
                                 waveFormat, nullptr);
    CoTaskMemFree(waveFormat);
    if (FAILED(hr)) {
        lastError_ = "Failed to initialize audio client";
        return -1;
    }
    
    // 6. Create buffer ready event
    bufferReadyEvent_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    hr = audioClient->SetEventHandle(bufferReadyEvent_);
    if (FAILED(hr)) {
        lastError_ = "Failed to set event handle";
        return -1;
    }
    
    // 7. Get render client
    ComPtr<IAudioRenderClient> renderClient;
    hr = audioClient->GetService(IID_PPV_ARGS(&renderClient));
    if (FAILED(hr)) {
        lastError_ = "Failed to get render client";
        return -1;
    }
    
    audioClient_ = audioClient;
    renderClient_ = renderClient;
    return 0;
}

int WASAPIBackend::startOutput() {
    HRESULT hr = audioClient_->Start();
    if (FAILED(hr)) {
        lastError_ = "Failed to start audio client";
        return -1;
    }
    
    running_ = true;
    audioThread_ = std::thread(&WASAPIBackend::audioThreadProc_, this);
    return 0;
}

void WASAPIBackend::audioThreadProc_() {
    while (running_) {
        DWORD waitResult = WaitForSingleObject(bufferReadyEvent_, 1000);
        
        if (waitResult == WAIT_OBJECT_0) {
            fillBuffer_();
        } else if (waitResult == WAIT_TIMEOUT) {
            // Device disconnected or no activity
            running_ = false;
        }
    }
}

int WASAPIBackend::fillBuffer_() {
    HRESULT hr;
    UINT32 bufferFrames, availableFrames;
    
    // Get number of frames available in buffer
    hr = audioClient_->GetBufferSize(&bufferFrames);
    if (FAILED(hr)) return -1;
    
    hr = audioClient_->GetCurrentPadding(&availableFrames);
    if (FAILED(hr)) return -1;
    
    UINT32 framesToWrite = bufferFrames - availableFrames;
    if (framesToWrite == 0) return 0;
    
    // Get render buffer
    BYTE* pData = nullptr;
    hr = renderClient_->GetBuffer(framesToWrite, &pData);
    if (FAILED(hr)) return -1;
    
    // Fill with audio (float format)
    float* floatData = (float*)pData;
    twSpeaker::renderFrames(floatData, framesToWrite * 2);  // Stereo
    
    // Release buffer
    hr = renderClient_->ReleaseBuffer(framesToWrite, 0);
    if (FAILED(hr)) return -1;
    
    return 0;
}

std::vector<AudioDevice> WASAPIBackend::enumerateDevices() const {
    std::vector<AudioDevice> devices;
    HRESULT hr = S_OK;
    
    ComPtr<IMMDeviceEnumerator> enumerator;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                         CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) return devices;
    
    ComPtr<IMMDeviceCollection> collection;
    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE,
                                       &collection);
    if (FAILED(hr)) return devices;
    
    UINT count;
    collection->GetCount(&count);
    
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        collection->Item(i, &device);
        
        LPWSTR deviceId;
        device->GetId(&deviceId);
        
        ComPtr<IPropertyStore> propStore;
        device->OpenPropertyStore(STGM_READ, &propStore);
        
        PROPVARIANT varName;
        PropVariantInit(&varName);
        propStore->GetValue(PKEY_Device_FriendlyName, &varName);
        
        AudioDevice ad;
        ad.id = WideCharToMultiByte(CP_UTF8, deviceId);
        ad.name = WideCharToMultiByte(CP_UTF8, varName.pwszVal);
        ad.maxOutputChannels = 2;
        devices.push_back(ad);
        
        PropVariantClear(&varName);
        CoTaskMemFree(deviceId);
    }
    
    return devices;
}
```

**Build Integration**:
```cmake
if(WIN32 AND ENABLE_WASAPI)
    target_link_libraries(tw303a PRIVATE ole32 oleaut32 uuid)
    target_sources(tw303a PRIVATE src/audio/wasapi_backend.cc)
    target_compile_definitions(tw303a PRIVATE QBX_WINDOWS_WASAPI=1)
endif()
```

**Native-rate support (wire format)**:
- `getConfig().sampleType` is already derived from the mix format
  (`detectSampleFormat`): float32 / int16 / int32 — surface it on `AudioConfig`.
- **Shared mode:** `supportedRates()` returns `{ mixFormat->nSamplesPerSec }`
  (the value `GetMixFormat` reports). `preferredRate` is honored only if it
  equals the mix rate; otherwise the speaker resampler bridges it.
- **Exclusive mode (future):** build a `WAVEFORMATEX` at `preferredRate` and
  probe with `IAudioClient::IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, …)`;
  `supportedRates()` enumerates the rates that pass. Honoring `preferredRate`
  then opens the endpoint exclusively at that rate — bit-perfect, no resampling.

**Dependencies**: Built-in (Windows SDK)

**Priority**: HIGH — Critical for Windows support

---

## Implementation Timeline

| Phase | Target | Timeline | Owner |
|-------|--------|----------|-------|
| **1. Audio Abstraction** | Define interface, refactor twspeaker.cc | Week 1-2 | Dev |
| **1b. Wire-format rate negotiation** | `supportedRates()` + `openDevice(preferredRate)` + `AudioConfig.sampleType`; fold into `twNegotiator`'s `D` and `twSpeaker` open/resampler-config | Week 2 | Dev |
| **2. ALSA Modernization** | Device enum, dynamic config, error recovery | Week 2-3 | Dev |
| **3. macOS CoreAudio** | Replace broken API, test on M1+Intel | Week 3-5 | Dev |
| **4. Windows WASAPI** | Full implementation, device enum | Week 4-6 | Dev |
| **5. PipeWire (optional)** | New backend, testing | Week 6-7 | Dev |
| **6. JACK (optional)** | Professional audio routing | Week 7-8 | Dev |
| **7. Device UI** | Device selection, config in UI | Week 8-9 | UI Dev |
| **8. Testing & Docs** | Cross-platform testing, user docs | Week 9-10 | QA/Dev |

## Testing Strategy

### Unit Tests
```cpp
// test/audio_backend_test.cc
TEST(ALSABackend, EnumerateDevices) {
    ALSABackend backend;
    auto devices = backend.enumerateDevices();
    EXPECT_GT(devices.size(), 0);
}

TEST(WASAPIBackend, OpenDefaultDevice) {
    WASAPIBackend backend;
    EXPECT_EQ(backend.openDevice("default"), 0);
    backend.closeDevice();
}
```

### Integration Tests
- Real audio output to speakers
- Multi-backend simultaneous operation
- Device plug/unplug handling
- Latency measurement

### Platform-Specific Testing
- **Linux**: ALSA, PipeWire, JACK, USB audio
- **macOS**: Intel + M1/M2/M3, various USB devices
- **Windows**: Windows 11, USB audio, virtual audio devices

## References

- **ALSA**: http://www.alsa-project.org/main/index.php/Documentation
- **PipeWire**: https://pipewire.org/docs/
- **JACK**: https://jackaudio.org/
- **CoreAudio**: https://developer.apple.com/documentation/coreaudio
- **WASAPI**: https://learn.microsoft.com/en-us/windows/win32/coreaudio/wasapi

## Risk Mitigation

| Risk | Impact | Mitigation |
|------|--------|-----------|
| ALSA changes break existing setup | HIGH | Keep fallback to old impl during transition |
| macOS API deprecated again | MEDIUM | Monitor Apple docs, use stable APIs only |
| Windows driver issues | MEDIUM | Test on multiple Windows 11 versions |
| JACK/PipeWire not installed | LOW | Make optional, detect at runtime |
| Latency regression | MEDIUM | Measure & benchmark all backends |

## Success Criteria

- ✅ All three platforms (Linux, macOS, Windows) build and run
- ✅ Audio output works without crackling/underruns
- ✅ Device enumeration functional
- ✅ Sample rate/format negotiation automatic
- ✅ Backend advertises native rates via `supportedRates()` and honors a
  requested rate via `openDevice(device, preferredRate)`; the negotiator folds
  these into `D` and the speaker resampler is a **passthrough whenever the
  device can open at the project rate**
- ✅ `getConfig().sampleType` drives the device-boundary conversion (no
  superfluous float↔int round-trips)
- ✅ Latency < 50ms on all platforms
- ✅ Professional audio workflows supported (JACK)
- ✅ Graceful fallbacks when backends unavailable
