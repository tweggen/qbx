#include "wasapi_input.h"

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

    const DWORD autoConvertFlags =
        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
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
            hr = audioClient_->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10000000,
                                           0, deviceFormat, nullptr);
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

    HRESULT hr = audioClient_->Start();
    if (FAILED(hr)) {
        lastError_ = "Failed to start audio capture";
        return -1;
    }

    state_ = WasapiInputState::Capturing;
    return 0;
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

    HRESULT hr = audioClient_->Stop();
    state_ = WasapiInputState::Open;

    if (FAILED(hr)) {
        lastError_ = "Failed to stop audio capture";
        return -1;
    }

    return 0;
}

std::int32_t WASAPIInput::read(float *interleaved, std::size_t frameCount) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != WasapiInputState::Capturing) {
        return 0;
    }

    UINT32 packetLength = 0;
    HRESULT hr = captureClient_->GetNextPacketSize(&packetLength);
    if (FAILED(hr)) {
        lastError_ = "Failed to get packet size";
        return -1;
    }

    if (packetLength == 0) {
        return 0;
    }

    BYTE *data = nullptr;
    UINT32 numFramesAvailable = 0;
    DWORD flags = 0;

    hr = captureClient_->GetBuffer(&data, &numFramesAvailable, &flags, nullptr, nullptr);
    if (FAILED(hr)) {
        lastError_ = "Failed to get capture buffer";
        return -1;
    }

    std::uint32_t framesToCopy = (numFramesAvailable < frameCount) ? numFramesAvailable
                                                                   : static_cast<std::uint32_t>(frameCount);

    if (data && (flags & AUDCLNT_BUFFERFLAGS_SILENT) == 0) {
        // Copy audio data
        std::memcpy(interleaved, data, framesToCopy * config_.channels * sizeof(float));
    } else {
        // Fill with silence
        std::memset(interleaved, 0, framesToCopy * config_.channels * sizeof(float));
    }

    captureClient_->ReleaseBuffer(numFramesAvailable);

    return static_cast<std::int32_t>(framesToCopy);
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
