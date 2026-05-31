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
#include <cstring>

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

namespace {

const REFERENCE_TIME kRequestedDurationHns = 100 * 10000;  // 100 ms

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
    if (running_.load()) stopOutput();
    closeDevice();
    if (comInitialized_) {
        CoUninitialize();
        comInitialized_ = false;
    }
}

int WASAPIBackend::openDevice(const std::string & /*deviceName*/,
                              std::uint32_t preferredRate)
{
    if (audioClient_) {
        syslog(LOG_WARNING, "WASAPIBackend::openDevice: already open");
        return 0;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr)) comInitialized_ = true;
    else if (hr != RPC_E_CHANGED_MODE && hr != S_FALSE) {
        syslog(LOG_ERR, "WASAPIBackend: CoInitializeEx failed: 0x%08lx", (unsigned long)hr);
        return -1;
    }

    hr = CoCreateInstance(CLSID_MMDeviceEnumerator_local, nullptr,
                          CLSCTX_INPROC_SERVER, IID_IMMDeviceEnumerator_local,
                          reinterpret_cast<void **>(&enumerator_));
    if (FAILED(hr)) {
        syslog(LOG_ERR, "WASAPIBackend: CoCreateInstance(MMDeviceEnumerator) failed: 0x%08lx",
               (unsigned long)hr);
        return -1;
    }

    hr = enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device_);
    if (FAILED(hr)) {
        syslog(LOG_ERR, "WASAPIBackend: GetDefaultAudioEndpoint failed: 0x%08lx",
               (unsigned long)hr);
        return -1;
    }

    hr = device_->Activate(IID_IAudioClient_local, CLSCTX_INPROC_SERVER, nullptr,
                           reinterpret_cast<void **>(&audioClient_));
    if (FAILED(hr)) {
        syslog(LOG_ERR, "WASAPIBackend: IMMDevice::Activate(IAudioClient) failed: 0x%08lx",
               (unsigned long)hr);
        return -1;
    }

    WAVEFORMATEX *mixFormat = nullptr;
    hr = audioClient_->GetMixFormat(&mixFormat);
    if (FAILED(hr) || !mixFormat) {
        syslog(LOG_ERR, "WASAPIBackend: GetMixFormat failed: 0x%08lx", (unsigned long)hr);
        return -1;
    }

    sampleFormat_      = detectSampleFormat(mixFormat);
    config_.sampleRate = mixFormat->nSamplesPerSec;
    config_.channels   = mixFormat->nChannels;

    if (sampleFormat_ == WasapiSampleFormat::Unknown) {
        syslog(LOG_ERR,
               "WASAPIBackend: unsupported mix format (tag=0x%04x bits=%u) — "
               "device wants something other than float32/int16/int32; bailing",
               mixFormat->wFormatTag, mixFormat->wBitsPerSample);
        CoTaskMemFree(mixFormat);
        return -1;
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
        syslog(LOG_INFO,
               "WASAPIBackend: requested %u Hz but shared-mode mix rate is %u Hz; "
               "the speaker will resample.",
               (unsigned) preferredRate, config_.sampleRate);
    }

    hr = audioClient_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                  AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                  kRequestedDurationHns, 0, mixFormat, nullptr);
    CoTaskMemFree(mixFormat);
    if (FAILED(hr)) {
        syslog(LOG_ERR, "WASAPIBackend: IAudioClient::Initialize failed: 0x%08lx",
               (unsigned long)hr);
        return -1;
    }

    bufferReady_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!bufferReady_) {
        syslog(LOG_ERR, "WASAPIBackend: CreateEvent failed");
        return -1;
    }
    hr = audioClient_->SetEventHandle(bufferReady_);
    if (FAILED(hr)) {
        syslog(LOG_ERR, "WASAPIBackend: SetEventHandle failed: 0x%08lx", (unsigned long)hr);
        return -1;
    }

    UINT32 bufferFrames = 0;
    audioClient_->GetBufferSize(&bufferFrames);
    config_.bufferFrames = bufferFrames;
    config_.periodFrames = bufferFrames;
    floatScratch_.assign(bufferFrames * config_.channels, 0.0f);

    hr = audioClient_->GetService(IID_IAudioRenderClient_local,
                                  reinterpret_cast<void **>(&renderClient_));
    if (FAILED(hr)) {
        syslog(LOG_ERR, "WASAPIBackend: GetService(IAudioRenderClient) failed: 0x%08lx",
               (unsigned long)hr);
        return -1;
    }

    const char *fmtName = "?";
    switch (sampleFormat_) {
        case WasapiSampleFormat::Float32: fmtName = "float32"; break;
        case WasapiSampleFormat::Int16:   fmtName = "int16";   break;
        case WasapiSampleFormat::Int32:   fmtName = "int32";   break;
        default: break;
    }
    syslog(LOG_INFO,
           "WASAPIBackend: opened default endpoint, %u Hz, %u ch, %s, buffer=%u frames",
           config_.sampleRate, config_.channels, fmtName, config_.bufferFrames);

    syslog(LOG_INFO,
           "WASAPIBackend: device mix rate is %u Hz; twSpeaker resamples the "
           "synth output to match.",
           config_.sampleRate);
    return 0;
}

std::vector<std::uint32_t> WASAPIBackend::supportedRates() const
{
    // Shared mode: the only rate we can run without the OS mixer resampling is
    // the current mix rate, known once the device is open. Before open, return
    // empty ("unknown"). Exclusive-mode enumeration via IsFormatSupported is
    // future work.
    if (audioClient_ && config_.sampleRate != 0)
        return { config_.sampleRate };
    return {};
}

int WASAPIBackend::closeDevice()
{
    if (renderClient_) { renderClient_->Release(); renderClient_ = nullptr; }
    if (audioClient_)  { audioClient_->Release();  audioClient_  = nullptr; }
    if (device_)       { device_->Release();       device_       = nullptr; }
    if (enumerator_)   { enumerator_->Release();   enumerator_   = nullptr; }
    if (bufferReady_)  { CloseHandle(bufferReady_); bufferReady_ = nullptr; }
    sampleFormat_ = WasapiSampleFormat::Unknown;
    return 0;
}

int WASAPIBackend::startOutput()
{
    if (!audioClient_ || running_.load()) return 0;

    // Pre-fill one buffer with silence so the device has data when it starts.
    BYTE *silence = nullptr;
    if (SUCCEEDED(renderClient_->GetBuffer(config_.bufferFrames, &silence))) {
        renderClient_->ReleaseBuffer(config_.bufferFrames, AUDCLNT_BUFFERFLAGS_SILENT);
    }

    HRESULT hr = audioClient_->Start();
    if (FAILED(hr)) {
        syslog(LOG_ERR, "WASAPIBackend: IAudioClient::Start failed: 0x%08lx",
               (unsigned long)hr);
        return -1;
    }

    stopFlag_.store(false);
    running_.store(true);
    thread_ = std::thread(&WASAPIBackend::audioThreadProc_, this);
    return 0;
}

int WASAPIBackend::stopOutput()
{
    if (!running_.load()) return 0;
    stopFlag_.store(true);
    if (bufferReady_) SetEvent(bufferReady_);  // wake the thread so it sees stopFlag_
    if (thread_.joinable()) thread_.join();
    running_.store(false);
    if (audioClient_) audioClient_->Stop();
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
