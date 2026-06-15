#include "audio/wasapi_backend.h"

#include "twconvert.h"
#include "twsyslog.h"

// WIN32_LEAN_AND_MEAN / NOMINMAX are provided globally by the top-level
// CMakeLists.txt — do not redefine here.
#include <windows.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>
#include <ksmedia.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

// MinGW omits KSDATAFORMAT_SUBTYPE_IEEE_FLOAT / _PCM from its ksmedia.h in
// some builds — define them locally if not already provided.
#ifndef KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
static const GUID KSDATAFORMAT_SUBTYPE_IEEE_FLOAT_local = {
    0x00000003, 0x0000, 0x0010,
    {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
#  define KSDATAFORMAT_SUBTYPE_IEEE_FLOAT KSDATAFORMAT_SUBTYPE_IEEE_FLOAT_local
#endif
#ifndef KSDATAFORMAT_SUBTYPE_PCM
static const GUID KSDATAFORMAT_SUBTYPE_PCM_local = {
    0x00000001, 0x0000, 0x0010,
    {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
#  define KSDATAFORMAT_SUBTYPE_PCM KSDATAFORMAT_SUBTYPE_PCM_local
#endif

// MinGW's headers don't always provide CLSID_MMDeviceEnumerator /
// IID_IMMDeviceEnumerator / IID_IAudioClient / IID_IAudioRenderClient as
// linker symbols. Define them locally from the documented GUIDs.
static const CLSID CLSID_MMDeviceEnumerator_local = {
    0xBCDE0395, 0xE52F, 0x467C,
    {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const IID IID_IMMDeviceEnumerator_local = {
    0xA95664D2, 0x9614, 0x4F35,
    {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const IID IID_IAudioClient_local = {
    0x1CB9AD4C, 0xDBFA, 0x4C32,
    {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
static const IID IID_IAudioRenderClient_local = {
    0xF294ACFC, 0x3146, 0x4483,
    {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};
// PKEY_Device_FriendlyName: also not always provided as a linker symbol by
// MinGW. {fmtid, pid} from functiondiscoverykeys_devpkey.h.
static const PROPERTYKEY PKEY_Device_FriendlyName_local = {
    {0xA45C254E, 0xDF1C, 0x4EFD,
     {0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0}}, 14};

namespace {

const REFERENCE_TIME kRequestedDurationHns = 100 * 10000;  // 100 ms

const char *stateName(audio::WasapiState s)
{
    switch (s) {
        case audio::WasapiState::Closed:   return "Closed";
        case audio::WasapiState::Opening:  return "Opening";
        case audio::WasapiState::Open:     return "Open";
        case audio::WasapiState::Starting: return "Starting";
        case audio::WasapiState::Running:  return "Running";
        case audio::WasapiState::Stopping: return "Stopping";
        case audio::WasapiState::Closing:  return "Closing";
    }
    return "?";
}

std::string wideToUtf8(const wchar_t *w)
{
    if (!w) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string s(static_cast<std::size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), len, nullptr, nullptr);
    return s;
}

std::wstring utf8ToWide(const std::string &s)
{
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 0) return {};
    std::wstring w(static_cast<std::size_t>(len - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), len);
    return w;
}

audio::WasapiSampleFormat detectSampleFormat(const WAVEFORMATEX *wf)
{
    if (!wf) return audio::WasapiSampleFormat::Unknown;

    if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && wf->wBitsPerSample == 32)
        return audio::WasapiSampleFormat::Float32;
    if (wf->wFormatTag == WAVE_FORMAT_PCM && wf->wBitsPerSample == 16)
        return audio::WasapiSampleFormat::Int16;
    if (wf->wFormatTag == WAVE_FORMAT_PCM && wf->wBitsPerSample == 32)
        return audio::WasapiSampleFormat::Int32;

    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE
        && wf->cbSize >= (sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))) {
        auto *ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(wf);
        if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)
            && wf->wBitsPerSample == 32)
            return audio::WasapiSampleFormat::Float32;
        if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) {
            if (wf->wBitsPerSample == 16) return audio::WasapiSampleFormat::Int16;
            if (wf->wBitsPerSample == 32) return audio::WasapiSampleFormat::Int32;
        }
    }
    return audio::WasapiSampleFormat::Unknown;
}

}  // namespace

namespace audio {

WASAPIBackend::WASAPIBackend() = default;

WASAPIBackend::~WASAPIBackend()
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    // closeDevice's logic, inline under the lock: stop a running stream, release
    // the handles, then balance the COM init. Going through the locked helpers
    // keeps the single teardown path (no second, lock-free variant to drift).
    if (state_ == WasapiState::Running) stopOutputLocked_();
    releaseDevice_();
    if (comInitialized_) {
        CoUninitialize();
        comInitialized_ = false;
    }
}

int WASAPIBackend::openDevice(const std::string &deviceName,
                              std::uint32_t preferredRate)
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    // A clean, explicit state replaces the old "which handles are set?" guesswork.
    // Already usable (Open or Running): nothing to do. Otherwise we must be Closed;
    // any failure below returns us to Closed via releaseDevice_(), so a
    // half-completed open can never leave a partially-set device behind.
    if (state_ == WasapiState::Open || state_ == WasapiState::Running) {
        fprintf(stderr, "WASAPIBackend::openDevice: already open (state=%s)\n",
                stateName(state_));
        return 0;
    }
    if (state_ != WasapiState::Closed) {
        // Only reachable if a transient state leaked, which the mutex prevents;
        // guard anyway so the contract is explicit.
        fprintf(stderr, "WASAPIBackend::openDevice: unexpected state %s\n",
                stateName(state_));
        return -1;
    }
    state_ = WasapiState::Opening;

    // Single failure exit: release whatever was created and fall back to Closed.
    auto fail = [this]() -> int {
        releaseDevice_();   // resets handles, sampleFormat_ and state_ to Closed
        return -1;
    };

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr)) comInitialized_ = true;
    else if (hr != RPC_E_CHANGED_MODE && hr != S_FALSE) {
        fprintf(stderr, "WASAPIBackend: CoInitializeEx failed: 0x%08lx\n", (unsigned long)hr);
        return fail();
    }

    hr = CoCreateInstance(CLSID_MMDeviceEnumerator_local, nullptr,
                          CLSCTX_INPROC_SERVER, IID_IMMDeviceEnumerator_local,
                          reinterpret_cast<void **>(&enumerator_));
    if (FAILED(hr)) {
        fprintf(stderr, "WASAPIBackend: CoCreateInstance(MMDeviceEnumerator) failed: 0x%08lx\n",
                (unsigned long)hr);
        return fail();
    }

    if (deviceName.empty() || deviceName == "default") {
        hr = enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device_);
        if (FAILED(hr)) {
            fprintf(stderr, "WASAPIBackend: GetDefaultAudioEndpoint failed: 0x%08lx\n",
                    (unsigned long)hr);
            return fail();
        }
    } else {
        std::wstring wid = utf8ToWide(deviceName);
        hr = enumerator_->GetDevice(wid.c_str(), &device_);
        if (FAILED(hr)) {
            fprintf(stderr,
                    "WASAPIBackend: GetDevice for selected endpoint failed "
                    "(0x%08lx); falling back to system default.\n",
                    (unsigned long)hr);
            hr = enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device_);
            if (FAILED(hr)) {
                fprintf(stderr, "WASAPIBackend: GetDefaultAudioEndpoint failed: 0x%08lx\n",
                        (unsigned long)hr);
                return fail();
            }
        }
    }

    hr = device_->Activate(IID_IAudioClient_local, CLSCTX_INPROC_SERVER, nullptr,
                           reinterpret_cast<void **>(&audioClient_));
    if (FAILED(hr)) {
        fprintf(stderr, "WASAPIBackend: IMMDevice::Activate(IAudioClient) failed: 0x%08lx\n",
                (unsigned long)hr);
        return fail();
    }

    WAVEFORMATEX *mixFormat = nullptr;
    hr = audioClient_->GetMixFormat(&mixFormat);
    if (FAILED(hr) || !mixFormat) {
        fprintf(stderr, "WASAPIBackend: GetMixFormat failed: 0x%08lx\n", (unsigned long)hr);
        return fail();
    }

    sampleFormat_      = detectSampleFormat(mixFormat);
    config_.sampleRate = mixFormat->nSamplesPerSec;
    config_.channels   = mixFormat->nChannels;

    if (sampleFormat_ == WasapiSampleFormat::Unknown) {
        fprintf(stderr,
                "WASAPIBackend: unsupported mix format (tag=0x%04x bits=%u) — "
                "device wants something other than float32/int16/int32; bailing\n",
                mixFormat->wFormatTag, mixFormat->wBitsPerSample);
        CoTaskMemFree(mixFormat);
        return fail();
    }

    // Report the device-native binary format on the config (wire format).
    switch (sampleFormat_) {
        case WasapiSampleFormat::Float32: config_.sampleType = twSampleType::Float32; break;
        case WasapiSampleFormat::Int16:   config_.sampleType = twSampleType::Int16;   break;
        case WasapiSampleFormat::Int32:   config_.sampleType = twSampleType::Int32;   break;
        default: break;
    }

    // Shared mode is locked to the OS mix rate, so a preferred rate that differs
    // can't be honoured here — the speaker resampler bridges it. (Exclusive-mode
    // support for honouring preferredRate natively is future work; see prop 02.)
    if (preferredRate != 0 && preferredRate != config_.sampleRate) {
        fprintf(stderr,
                "WASAPIBackend: requested %u Hz but shared-mode mix rate is %u Hz; "
                "the speaker will resample.\n",
                (unsigned) preferredRate, config_.sampleRate);
    }

    hr = audioClient_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                  AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                  kRequestedDurationHns, 0, mixFormat, nullptr);
    CoTaskMemFree(mixFormat);
    if (FAILED(hr)) {
        fprintf(stderr, "WASAPIBackend: IAudioClient::Initialize failed: 0x%08lx\n",
                (unsigned long)hr);
        return fail();
    }

    bufferReady_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!bufferReady_) {
        fprintf(stderr, "WASAPIBackend: CreateEvent failed\n");
        return fail();
    }
    hr = audioClient_->SetEventHandle(bufferReady_);
    if (FAILED(hr)) {
        fprintf(stderr, "WASAPIBackend: SetEventHandle failed: 0x%08lx\n", (unsigned long)hr);
        return fail();
    }

    UINT32 bufferFrames = 0;
    audioClient_->GetBufferSize(&bufferFrames);
    config_.bufferFrames = bufferFrames;
    config_.periodFrames = bufferFrames;
    floatScratch_.assign(bufferFrames * config_.channels, 0.0f);

    // Query output latency (device + driver + OS buffering).
    REFERENCE_TIME latencyHns = 0;
    hr = audioClient_->GetStreamLatency(&latencyHns);
    if (SUCCEEDED(hr) && latencyHns > 0) {
        // Convert from 100ns units to frames: latency_frames = latency_hns * sampleRate / 10_000_000
        config_.outputLatencyFrames = static_cast<uint32_t>(
            (latencyHns * config_.sampleRate) / 10000000LL
        );
        fprintf(stderr,
                "WASAPIBackend: output latency %.2f ms (%u frames @ %u Hz)\n",
                latencyHns / 10000.0, config_.outputLatencyFrames, config_.sampleRate);
    } else {
        config_.outputLatencyFrames = 0;
    }

    hr = audioClient_->GetService(IID_IAudioRenderClient_local,
                                  reinterpret_cast<void **>(&renderClient_));
    if (FAILED(hr)) {
        fprintf(stderr, "WASAPIBackend: GetService(IAudioRenderClient) failed: 0x%08lx\n",
                (unsigned long)hr);
        return fail();
    }

    const char *fmtName = "?";
    switch (sampleFormat_) {
        case WasapiSampleFormat::Float32: fmtName = "float32"; break;
        case WasapiSampleFormat::Int16:   fmtName = "int16";   break;
        case WasapiSampleFormat::Int32:   fmtName = "int32";   break;
        default: break;
    }
    fprintf(stderr,
            "WASAPIBackend: opened default endpoint, %u Hz, %u ch, %s, buffer=%u frames\n",
            config_.sampleRate, config_.channels, fmtName, config_.bufferFrames);

    fprintf(stderr,
            "WASAPIBackend: device mix rate is %u Hz; twSpeaker resamples the "
            "synth output to match.\n",
            config_.sampleRate);

    state_ = WasapiState::Open;
    return 0;
}

std::vector<AudioDeviceInfo> WASAPIBackend::enumerateDevices() const
{
    std::vector<AudioDeviceInfo> devices;
    devices.push_back({ "default", "System default" });

    // Ensure COM is available on this (GUI) thread; balance only what we init.
    HRESULT ci = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool didInit = (ci == S_OK || ci == S_FALSE);

    // Use a transient enumerator so this works before openDevice().
    IMMDeviceEnumerator *enumr = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_MMDeviceEnumerator_local, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_IMMDeviceEnumerator_local,
                                  reinterpret_cast<void **>(&enumr));
    if (FAILED(hr) || !enumr) {
        if (didInit) CoUninitialize();
        return devices;
    }

    IMMDeviceCollection *coll = nullptr;
    hr = enumr->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll);
    if (SUCCEEDED(hr) && coll) {
        UINT count = 0;
        coll->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            IMMDevice *dev = nullptr;
            if (FAILED(coll->Item(i, &dev)) || !dev) continue;

            LPWSTR wid = nullptr;
            std::string id, label;
            if (SUCCEEDED(dev->GetId(&wid)) && wid) {
                id = wideToUtf8(wid);
                CoTaskMemFree(wid);
            }

            IPropertyStore *props = nullptr;
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)) && props) {
                PROPVARIANT name;
                PropVariantInit(&name);
                if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName_local, &name))
                    && name.vt == VT_LPWSTR) {
                    label = wideToUtf8(name.pwszVal);
                }
                PropVariantClear(&name);
                props->Release();
            }
            if (!id.empty())
                devices.push_back({ id, label.empty() ? id : label });
            dev->Release();
        }
        coll->Release();
    }
    enumr->Release();
    if (didInit) CoUninitialize();
    return devices;
}

bool WASAPIBackend::isRunning() const
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    return state_ == WasapiState::Running;
}

void WASAPIBackend::setRenderCallback(RenderCallback cb)
{
    // Guarded by the same lock as the lifecycle: the audio thread reads callback_
    // without locking, so it must only ever be assigned while no thread is running
    // (state_ != Running). The lock also publishes the new callback safely.
    std::lock_guard<std::mutex> lock(stateMutex_);
    callback_ = std::move(cb);
}

AudioConfig WASAPIBackend::getConfig() const
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    return config_;
}

std::vector<std::uint32_t> WASAPIBackend::supportedRates() const
{
    // Shared mode: the only rate we can run without the OS mixer resampling is
    // the current mix rate, known once the device is open. Before open, return
    // empty ("unknown"). Exclusive-mode enumeration via IsFormatSupported is
    // future work.
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (state_ != WasapiState::Closed && config_.sampleRate != 0)
        return { config_.sampleRate };
    return {};
}

void WASAPIBackend::releaseDevice_()
{
    // Caller holds stateMutex_. Only legal from a non-Running state — stopOutput
    // must have joined the audio thread first, so no one is touching these handles.
    if (renderClient_) { renderClient_->Release(); renderClient_ = nullptr; }
    if (audioClient_)  { audioClient_->Release();  audioClient_  = nullptr; }
    if (device_)       { device_->Release();       device_       = nullptr; }
    if (enumerator_)   { enumerator_->Release();   enumerator_   = nullptr; }
    if (bufferReady_)  { CloseHandle(bufferReady_); bufferReady_ = nullptr; }
    sampleFormat_ = WasapiSampleFormat::Unknown;
    state_ = WasapiState::Closed;
}

int WASAPIBackend::closeDevice()
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (state_ == WasapiState::Closed) return 0;
    // Stop a running stream first (joins the audio thread) so releaseDevice_ never
    // frees handles out from under it.
    if (state_ == WasapiState::Running) stopOutputLocked_();
    state_ = WasapiState::Closing;
    releaseDevice_();   // transitions to Closed
    return 0;
}

int WASAPIBackend::startOutput()
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    return startOutputLocked_();
}

int WASAPIBackend::startOutputLocked_()
{
    // Caller holds stateMutex_.
    if (state_ == WasapiState::Running) return 0;
    if (state_ != WasapiState::Open) {
        fprintf(stderr,
                "WASAPIBackend::startOutput: device not open (state=%s)\n",
                stateName(state_));
        return -1;
    }
    // Reap a previous audio thread that already exited but was never joined:
    // assigning over a joinable std::thread calls std::terminate. (After a clean
    // stopOutput the thread is already joined; this guards the defensive cases.)
    if (thread_.joinable()) thread_.join();

    state_ = WasapiState::Starting;

    // Pre-fill one buffer with silence so the device has data when it starts.
    BYTE *silence = nullptr;
    if (SUCCEEDED(renderClient_->GetBuffer(config_.bufferFrames, &silence))) {
        renderClient_->ReleaseBuffer(config_.bufferFrames, AUDCLNT_BUFFERFLAGS_SILENT);
    }

    HRESULT hr = audioClient_->Start();
    if (FAILED(hr)) {
        fprintf(stderr, "WASAPIBackend: IAudioClient::Start failed: 0x%08lx\n",
                (unsigned long)hr);
        state_ = WasapiState::Open;   // device still open, just not running
        return -1;
    }

    // Publish stopFlag_=false before the thread starts so it never observes a
    // stale stop request from a prior run.
    stopFlag_.store(false);
    thread_ = std::thread(&WASAPIBackend::audioThreadProc_, this);
    state_ = WasapiState::Running;
    return 0;
}

int WASAPIBackend::stopOutput()
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    return stopOutputLocked_();
}

int WASAPIBackend::stopOutputLocked_()
{
    // Caller holds stateMutex_.
    if (state_ != WasapiState::Running) return 0;

    state_ = WasapiState::Stopping;
    stopFlag_.store(true);
    if (bufferReady_) SetEvent(bufferReady_);  // wake the thread so it sees stopFlag_
    // Join the audio thread before touching the device. joinable() is false after
    // a prior join, so this is safe against double-stop; the state_ check above
    // serialises concurrent callers. The audio thread never takes stateMutex_, so
    // joining under the lock can't deadlock.
    if (thread_.joinable()) thread_.join();
    if (audioClient_) audioClient_->Stop();
    state_ = WasapiState::Open;
    return 0;
}

void WASAPIBackend::audioThreadProc_()
{
    // Boost thread priority for low-latency audio. Best-effort.
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    while (!stopFlag_.load()) {
        DWORD wait = WaitForSingleObject(bufferReady_, 2000);
        if (stopFlag_.load()) break;
        if (wait != WAIT_OBJECT_0) continue;
        if (renderOnce_() < 0) break;
    }

    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
}

int WASAPIBackend::renderOnce_()
{
    UINT32 padding = 0;
    HRESULT hr = audioClient_->GetCurrentPadding(&padding);
    if (FAILED(hr)) return -1;

    UINT32 framesAvail = config_.bufferFrames - padding;
    if (framesAvail == 0) return 0;

    BYTE *dst = nullptr;
    hr = renderClient_->GetBuffer(framesAvail, &dst);
    if (FAILED(hr) || !dst) return -1;

    // Pull mono / N-channel float from the synth into our scratch buffer.
    std::size_t framesProduced = 0;
    if (callback_) {
        framesProduced = callback_(floatScratch_.data(), framesAvail, config_.channels);
    }
    if (framesProduced < framesAvail) {
        std::fill(floatScratch_.begin() + framesProduced * config_.channels,
                  floatScratch_.begin() + framesAvail   * config_.channels,
                  0.0f);
    }

    twSampleType dstType;
    switch (sampleFormat_) {
        case WasapiSampleFormat::Float32: dstType = twSampleType::Float32; break;
        case WasapiSampleFormat::Int16:   dstType = twSampleType::Int16;   break;
        case WasapiSampleFormat::Int32:   dstType = twSampleType::Int32;   break;
        default:
            renderClient_->ReleaseBuffer(framesAvail, AUDCLNT_BUFFERFLAGS_SILENT);
            return -1;
    }

    // Interleaved N-channel float scratch → device's native binary format.
    // Same channel count and layout, so this is purely a sample-type conversion
    // (and a plain memcpy in the float32 case).
    twFormat srcFmt;
    srcFmt.sampleType = twSampleType::Float32;
    srcFmt.channels   = static_cast<std::uint16_t>(config_.channels);
    twFormat dstFmt = srcFmt;
    dstFmt.sampleType = dstType;
    twConvertFrames(srcFmt, floatScratch_.data(), dstFmt, dst,
                    static_cast<length_t>(framesAvail));

    renderClient_->ReleaseBuffer(framesAvail, 0);
    return 0;
}

}  // namespace audio
