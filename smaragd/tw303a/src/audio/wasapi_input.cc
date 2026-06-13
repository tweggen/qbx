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
    closeDevice();
}

int WASAPIInput::openDevice(const std::string &deviceId, std::uint32_t preferredRate) {
    HRESULT hr;

    // Initialize COM
    hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        lastError_ = "Failed to initialize COM";
        return -1;
    }
    comInitialized_ = true;

    // Create device enumerator. From here on, every failure routes through
    // closeDevice(), which releases whatever was created, nulls the pointers
    // (so ~WASAPIInput can't double-release a dangling COM pointer — the crash
    // this replaces) and uninitializes COM exactly once.
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), (void **)&enumerator_);
    if (FAILED(hr)) {
        lastError_ = "Failed to create device enumerator";
        closeDevice();
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
        closeDevice();
        return -1;
    }

    // Activate audio client
    hr = inputDevice_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                 (void **)&audioClient_);
    if (FAILED(hr)) {
        lastError_ = "Failed to activate audio client";
        closeDevice();
        return -1;
    }

    // Get device format
    WAVEFORMATEX *deviceFormat = nullptr;
    hr = audioClient_->GetMixFormat(&deviceFormat);
    if (FAILED(hr)) {
        lastError_ = "Failed to get device format";
        closeDevice();
        return -1;
    }

    // Shared-mode capture is LOCKED to the device's mix format — we must pass it
    // to Initialize() unmodified. (Overwriting nSamplesPerSec here left the rest
    // of the WAVEFORMATEX inconsistent and made Initialize() fail with
    // AUDCLNT_E_UNSUPPORTED_FORMAT.) We therefore capture at the device rate and
    // report it via getConfig(); the caller resamples to the desired rate.
    // preferredRate is intentionally not applied to the device here.
    (void)preferredRate;
    config_.sampleRate = deviceFormat->nSamplesPerSec;
    config_.channels = deviceFormat->nChannels;

    // Initialize audio client for capture
    hr = audioClient_->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10000000,  // 1 second buffer
                                   0, deviceFormat, nullptr);
    if (FAILED(hr)) {
        lastError_ = "Failed to initialize audio client";
        CoTaskMemFree(deviceFormat);
        closeDevice();
        return -1;
    }

    CoTaskMemFree(deviceFormat);

    // Get capture client
    hr = audioClient_->GetService(__uuidof(IAudioCaptureClient), (void **)&captureClient_);
    if (FAILED(hr)) {
        lastError_ = "Failed to get capture client";
        closeDevice();
        return -1;
    }

    return 0;
}

int WASAPIInput::closeDevice() {
    stopCapture();

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
    return 0;
}

int WASAPIInput::startCapture() {
    if (!audioClient_) {
        lastError_ = "Audio client not initialized";
        return -1;
    }

    HRESULT hr = audioClient_->Start();
    if (FAILED(hr)) {
        lastError_ = "Failed to start audio capture";
        return -1;
    }

    isCapturing_ = true;
    return 0;
}

int WASAPIInput::stopCapture() {
    if (!audioClient_ || !isCapturing_) {
        return 0;
    }

    HRESULT hr = audioClient_->Stop();
    isCapturing_ = false;

    if (FAILED(hr)) {
        lastError_ = "Failed to stop audio capture";
        return -1;
    }

    return 0;
}

std::int32_t WASAPIInput::read(float *interleaved, std::size_t frameCount) {
    if (!captureClient_ || !isCapturing_) {
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
    std::vector<AudioInputDeviceInfo> devices;

    HRESULT hr;
    IMMDeviceCollection *collection = nullptr;

    if (!enumerator_) {
        return devices;  // Return empty if not initialized
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
