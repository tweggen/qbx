#include "wasapi_input.h"
#include "tw/core/twlog.h"

#include <wrl.h>
#include <comdef.h>
#include <functiondiscoverykeys_devpkey.h>
#include <cstring>
#include <sstream>

using namespace Microsoft::WRL;

namespace audio {

// PKEY_Device_FriendlyName is not always provided as a linker symbol by MinGW.
// {fmtid, pid} from functiondiscoverykeys_devpkey.h (matches wasapi_backend.cc).
static const PROPERTYKEY PKEY_Device_FriendlyName_local = {
    {0xA45C254E, 0xDF1C, 0x4EFD,
     {0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0}}, 14};

WASAPIInput::WASAPIInput() {
    config_.sampleRate = 48000;
    config_.channels = 2;
    config_.bufferFrames = 1024;
    config_.sampleType = twSampleType::Float32;
}

WASAPIInput::~WASAPIInput() {
    std::lock_guard<std::mutex> lock(mutex_);
    closeDeviceLocked_();
}

int WASAPIInput::openDevice(const std::string &deviceId, std::uint32_t preferredRate) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Explicit state replaces the old "is this handle set?" reasoning. Already
    // open: nothing to do. Otherwise we must be Closed; any failure below routes
    // through closeDeviceLocked_(), which releases whatever was created, nulls the
    // pointers (so the destructor can't double-release a dangling COM pointer —
    // the crash this replaces), uninitializes COM exactly once and returns us to
    // Closed.
    if (state_ != WasapiInputState::Closed) {
        lastError_ = "Input device already open";
        return 0;
    }

    HRESULT hr;

    // Initialize COM
    hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        lastError_ = "Failed to initialize COM";
        return -1;
    }
    comInitialized_ = true;

    // Create device enumerator.
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), (void **)&enumerator_);
    if (FAILED(hr)) {
        lastError_ = "Failed to create device enumerator";
        closeDeviceLocked_();
        return -1;
    }

    // Get input device
    if (deviceId == "default") {
        hr = enumerator_->GetDefaultAudioEndpoint(eCapture, eMultimedia, &inputDevice_);
    } else {
        // Convert string ID to wide string and get device
        int len = MultiByteToWideChar(CP_UTF8, 0, deviceId.c_str(), -1, nullptr, 0);
        wchar_t *wideName = new wchar_t[len];
        MultiByteToWideChar(CP_UTF8, 0, deviceId.c_str(), -1, wideName, len);

        hr = enumerator_->GetDevice(wideName, &inputDevice_);
        delete[] wideName;
    }

    if (FAILED(hr)) {
        lastError_ = "Failed to get input device";
        closeDeviceLocked_();
        return -1;
    }

    // Activate audio client
    hr = inputDevice_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                 (void **)&audioClient_);
    if (FAILED(hr)) {
        lastError_ = "Failed to activate audio client";
        closeDeviceLocked_();
        return -1;
    }

    // Get device format
    WAVEFORMATEX *deviceFormat = nullptr;
    hr = audioClient_->GetMixFormat(&deviceFormat);
    if (FAILED(hr)) {
        lastError_ = "Failed to get device format";
        closeDeviceLocked_();
        return -1;
    }

    const std::uint32_t nativeRate = deviceFormat->nSamplesPerSec;
    const std::uint32_t wantRate   = preferredRate > 0 ? preferredRate : nativeRate;

    // Ask Windows to hand us frames at the rate WE want (the project rate) using
    // shared-mode automatic format conversion, instead of capturing at the device
    // mix rate and resampling ourselves. This removes the "what rate is the device
    // actually delivering?" guesswork that produced takes pitched off by the
    // 44.1/48k ratio: GetMixFormat's reported rate could disagree with the rate the
    // shared engine streamed, and our software resampler then double-converted.
    // With AUTOCONVERTPCM the engine guarantees the requested rate, so no resample
    // is needed downstream. Only nSamplesPerSec / nAvgBytesPerSec change; block
    // align (channels x bytes/sample) and the sub-format are untouched, keeping a
    // WAVEFORMATEXTENSIBLE (likely on this 16-ch endpoint) internally consistent.
    deviceFormat->nSamplesPerSec  = wantRate;
    deviceFormat->nAvgBytesPerSec = wantRate * deviceFormat->nBlockAlign;

    // AUDCLNT_STREAMFLAGS_EVENTCALLBACK (proposal 21 L0, design D5 / F7): the
    // client signals an event every period and our capture thread waits on it,
    // instead of a consumer polling GetNextPacketSize on a 1 ms sleep. Shared
    // mode requires hnsPeriodicity == 0 with this flag, which is what the
    // fourth argument already is.
    const DWORD autoConvertFlags =
        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY |
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    hr = audioClient_->Initialize(AUDCLNT_SHAREMODE_SHARED, autoConvertFlags,
                                   10000000,  // 1 second buffer
                                   0, deviceFormat, nullptr);
    if (SUCCEEDED(hr)) {
        config_.sampleRate = wantRate;
    } else {
        // Driver/endpoint refused auto-conversion. Fall back to capturing at the
        // native mix rate and reporting it, so the caller's resampler converts to
        // the project rate (the historical path). Restore the native rate on the
        // format before retrying, then re-activate a clean client (a failed
        // Initialize leaves the client unusable for a second Initialize).
        deviceFormat->nSamplesPerSec  = nativeRate;
        deviceFormat->nAvgBytesPerSec = nativeRate * deviceFormat->nBlockAlign;

        audioClient_->Release();
        audioClient_ = nullptr;
        hr = inputDevice_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                    (void **)&audioClient_);
        if (SUCCEEDED(hr)) {
            hr = audioClient_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                           AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                           10000000, 0, deviceFormat, nullptr);
        }
        if (FAILED(hr)) {
            lastError_ = "Failed to initialize audio client";
            CoTaskMemFree(deviceFormat);
            closeDeviceLocked_();
            return -1;
        }
        config_.sampleRate = nativeRate;
    }

    config_.channels = deviceFormat->nChannels;

    CoTaskMemFree(deviceFormat);

    // Get capture client
    hr = audioClient_->GetService(__uuidof(IAudioCaptureClient), (void **)&captureClient_);
    if (FAILED(hr)) {
        lastError_ = "Failed to get capture client";
        closeDeviceLocked_();
        return -1;
    }

    // The capture event the client signals each period. Auto-reset and
    // initially unsignalled; the capture thread is the only waiter.
    captureEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!captureEvent_) {
        lastError_ = "Failed to create capture event";
        closeDeviceLocked_();
        return -1;
    }
    hr = audioClient_->SetEventHandle((HANDLE) captureEvent_);
    if (FAILED(hr)) {
        lastError_ = "Failed to set capture event handle";
        closeDeviceLocked_();
        return -1;
    }

    // Ring size. The device buffer asked for is one second; the ring only has
    // to cover the gap between the device period and the consumer's poll, and a
    // consumer a whole second behind has a problem no buffer size fixes. The
    // floor of 16384 frames is ~340 ms at 48 kHz.
    {
        UINT32 bufferFrames = 0;
        if (SUCCEEDED(audioClient_->GetBufferSize(&bufferFrames)) && bufferFrames)
            config_.bufferFrames = bufferFrames;
        std::size_t want = (std::size_t) config_.bufferFrames * 4;
        if (want < 16384) want = 16384;
        ring_.reset(config_.channels, want);
    }

    // Query input latency (device + driver + OS buffering).
    REFERENCE_TIME latencyHns = 0;
    hr = audioClient_->GetStreamLatency(&latencyHns);
    if (SUCCEEDED(hr) && latencyHns > 0) {
        // Convert from 100ns units to frames: latency_frames = latency_hns * sampleRate / 10_000_000
        config_.inputLatencyFrames = static_cast<uint32_t>(
            (latencyHns * config_.sampleRate) / 10000000LL
        );
        TW_LOGD( "devices",
                "WASAPIInput: input latency %.2f ms (%u frames @ %u Hz)",
                latencyHns / 10000.0, config_.inputLatencyFrames, config_.sampleRate );
    } else {
        config_.inputLatencyFrames = 0;
    }

    state_ = WasapiInputState::Open;
    return 0;
}

int WASAPIInput::closeDevice() {
    std::lock_guard<std::mutex> lock(mutex_);
    closeDeviceLocked_();
    return 0;
}

void WASAPIInput::closeDeviceLocked_() {
    // Caller holds mutex_. Stop capture first so the device isn't running while we
    // release its services, then tear everything down and return to Closed.
    stopCaptureLocked_();

    if (captureClient_) {
        captureClient_->Release();
        captureClient_ = nullptr;
    }

    if (audioClient_) {
        audioClient_->Release();
        audioClient_ = nullptr;
    }

    if (inputDevice_) {
        inputDevice_->Release();
        inputDevice_ = nullptr;
    }

    if (enumerator_) {
        enumerator_->Release();
        enumerator_ = nullptr;
    }

    // After stopCaptureLocked_() joined the capture thread, so nothing can be
    // waiting on it.
    if (captureEvent_) {
        CloseHandle((HANDLE) captureEvent_);
        captureEvent_ = nullptr;
    }

    // Only uninitialize COM if *we* initialized it, and only once. closeDevice()
    // is called explicitly, again from ~WASAPIInput, and as openDevice()'s
    // failure-cleanup path; an unconditional CoUninitialize() would unbalance
    // COM's per-thread ref count (and a prior call already nulled the pointers).
    if (comInitialized_) {
        CoUninitialize();
        comInitialized_ = false;
    }

    state_ = WasapiInputState::Closed;
}

int WASAPIInput::startCapture() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == WasapiInputState::Capturing) return 0;
    if (state_ != WasapiInputState::Open) {
        lastError_ = "Audio client not initialized";
        return -1;
    }

    ring_.clear();
    ring_.resetStats();
    wakeups_.store(0, std::memory_order_relaxed);

    // The thread first, then the stream: a capture thread waiting on an event
    // nobody signals yet costs one timeout iteration, whereas a running stream
    // with nobody draining it overruns the device buffer.
    captureRun_.store(true, std::memory_order_release);
    captureThread_ = std::thread([this] { captureThreadMain_(); });

    HRESULT hr = audioClient_->Start();
    if (FAILED(hr)) {
        captureRun_.store(false, std::memory_order_release);
        SetEvent((HANDLE) captureEvent_);
        captureThread_.join();
        lastError_ = "Failed to start audio capture";
        return -1;
    }

    state_ = WasapiInputState::Capturing;
    return 0;
}

// The capture thread (proposal 21 L0). It takes NO lock — stopCapture() joins
// it while holding mutex_ — touches only captureClient_ and the ring, and
// pushes every packet WHOLE. That last point is the fix for design F7: the old
// read() released a packet after copying only as much as its caller's buffer
// held, so the tail of a larger packet was dropped on the floor and the
// recorded timeline silently compressed.
void WASAPIInput::captureThreadMain_() {
    tw::TwLog::markNonBlocking();
    tw::TwLog::nameThread( "audio-in-wasapi" );

    // Per-thread apartment for the client interfaces used below.
    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comHere = SUCCEEDED(comHr);

    std::vector<float> silence;

    while (captureRun_.load(std::memory_order_acquire)) {
        // A timeout rather than INFINITE, so a device that stops signalling
        // (unplugged, format change) cannot wedge the join. 200 ms is far more
        // than any shared-mode period.
        const DWORD w = WaitForSingleObject((HANDLE) captureEvent_, 200);
        if (!captureRun_.load(std::memory_order_acquire)) break;
        if (w != WAIT_OBJECT_0) continue;

        // Drain EVERY packet the client holds, not just the first: one event can
        // cover several, and a packet left behind is latency that never comes
        // back.
        for (;;) {
            UINT32 packetLength = 0;
            if (FAILED(captureClient_->GetNextPacketSize(&packetLength))) break;
            if (packetLength == 0) break;

            BYTE *data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(captureClient_->GetBuffer(&data, &frames, &flags,
                                                 nullptr, nullptr)))
                break;

            if (frames > 0) {
                if (data && (flags & AUDCLNT_BUFFERFLAGS_SILENT) == 0) {
                    ring_.push((const float *) data, frames);
                } else {
                    // A SILENT packet still occupies time: pushing zeros keeps
                    // the recorded timeline as long as the real one.
                    const std::size_t need =
                        (std::size_t) frames * config_.channels;
                    if (silence.size() < need) silence.assign(need, 0.0f);
                    ring_.push(silence.data(), frames);
                }
                wakeups_.fetch_add(1, std::memory_order_relaxed);
            }
            captureClient_->ReleaseBuffer(frames);
        }
    }

    if (comHere) CoUninitialize();
}

int WASAPIInput::stopCapture() {
    std::lock_guard<std::mutex> lock(mutex_);
    return stopCaptureLocked_();
}

int WASAPIInput::stopCaptureLocked_() {
    // Caller holds mutex_.
    if (state_ != WasapiInputState::Capturing) {
        return 0;
    }

    // The thread first: it is the only user of captureClient_, and stopping the
    // stream underneath it would leave it draining a client that is no longer
    // running. It never takes mutex_, so joining under the lock is safe.
    captureRun_.store(false, std::memory_order_release);
    if (captureEvent_) SetEvent((HANDLE) captureEvent_);
    if (captureThread_.joinable()) captureThread_.join();

    HRESULT hr = audioClient_->Stop();
    state_ = WasapiInputState::Open;

    if (FAILED(hr)) {
        lastError_ = "Failed to stop audio capture";
        return -1;
    }

    return 0;
}

std::int32_t WASAPIInput::read(float *interleaved, std::size_t frameCount) {
    // A RING POP now, not a device poll: no lock, no COM call, no packet
    // release. Whatever the device produced is already in the ring, so a caller
    // asking for fewer frames than arrived keeps the rest for its next call
    // instead of losing it (design F7).
    if (!interleaved) return -1;
    return (std::int32_t) ring_.pop(interleaved, frameCount);
}

AudioInputStats WASAPIInput::stats() const {
    AudioInputStats s;
    s.framesPushed = ring_.framesPushed();
    s.framesPopped = ring_.framesPopped();
    s.overrunFrames = ring_.overrunFrames();
    s.underrunFrames = ring_.underrunFrames();
    s.captureWakeups = wakeups_.load(std::memory_order_relaxed);
    return s;
}

const AudioInputConfig &WASAPIInput::getConfig() const {
    return config_;
}

std::vector<AudioInputDeviceInfo> WASAPIInput::listDevices() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<AudioInputDeviceInfo> devices;

    HRESULT hr;
    IMMDeviceCollection *collection = nullptr;

    if (!enumerator_) {
        return devices;  // Return empty if not initialized (device not open)
    }

    hr = enumerator_->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr)) {
        return devices;
    }

    UINT count = 0;
    collection->GetCount(&count);

    for (UINT i = 0; i < count; ++i) {
        IMMDevice *device = nullptr;
        hr = collection->Item(i, &device);
        if (FAILED(hr)) continue;

        LPWSTR id = nullptr;
        device->GetId(&id);

        IPropertyStore *props = nullptr;
        device->OpenPropertyStore(STGM_READ, &props);

        PROPVARIANT varName;
        PropVariantInit(&varName);
        props->GetValue(PKEY_Device_FriendlyName_local, &varName);

        // Convert wide string to UTF-8
        int len = WideCharToMultiByte(CP_UTF8, 0, varName.pwszVal, -1, nullptr, 0, nullptr, nullptr);
        std::string name(len - 1, 0);
        WideCharToMultiByte(CP_UTF8, 0, varName.pwszVal, -1, &name[0], len, nullptr, nullptr);

        len = WideCharToMultiByte(CP_UTF8, 0, id, -1, nullptr, 0, nullptr, nullptr);
        std::string deviceId(len - 1, 0);
        WideCharToMultiByte(CP_UTF8, 0, id, -1, &deviceId[0], len, nullptr, nullptr);

        devices.push_back({deviceId, name, 2});  // TODO: Get actual channel count

        PropVariantClear(&varName);
        props->Release();
        CoTaskMemFree(id);
        device->Release();
    }

    collection->Release();
    return devices;
}

const char *WASAPIInput::errorMessage() const {
    return lastError_.c_str();
}

}  // namespace audio
