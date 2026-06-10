#include "wasapi_input.h"

#include <wrl.h>
#include <comdef.h>
#include <cstring>
#include <sstream>

using namespace Microsoft::WRL;

namespace audio {

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

    // Create device enumerator
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), (void **)&enumerator_);
    if (FAILED(hr)) {
        lastError_ = "Failed to create device enumerator";
        CoUninitialize();
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
        if (enumerator_) enumerator_->Release();
        CoUninitialize();
        return -1;
    }

    // Activate audio client
    hr = inputDevice_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                 (void **)&audioClient_);
    if (FAILED(hr)) {
        lastError_ = "Failed to activate audio client";
        inputDevice_->Release();
        enumerator_->Release();
        CoUninitialize();
        return -1;
    }

    // Get device format
    WAVEFORMATEX *deviceFormat = nullptr;
    hr = audioClient_->GetMixFormat(&deviceFormat);
    if (FAILED(hr)) {
        lastError_ = "Failed to get device format";
        audioClient_->Release();
        inputDevice_->Release();
        enumerator_->Release();
        CoUninitialize();
        return -1;
    }

    // Apply preferred sample rate if specified
    if (preferredRate > 0) {
        config_.sampleRate = preferredRate;
        deviceFormat->nSamplesPerSec = preferredRate;
    } else {
        config_.sampleRate = deviceFormat->nSamplesPerSec;
    }

    config_.channels = deviceFormat->nChannels;

    // Initialize audio client for capture
    hr = audioClient_->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10000000,  // 1 second buffer
                                   0, deviceFormat, nullptr);
    if (FAILED(hr)) {
        lastError_ = "Failed to initialize audio client";
        CoTaskMemFree(deviceFormat);
        audioClient_->Release();
        inputDevice_->Release();
        enumerator_->Release();
        CoUninitialize();
        return -1;
    }

    CoTaskMemFree(deviceFormat);

    // Get capture client
    hr = audioClient_->GetService(__uuidof(IAudioCaptureClient), (void **)&captureClient_);
    if (FAILED(hr)) {
        lastError_ = "Failed to get capture client";
        audioClient_->Release();
        inputDevice_->Release();
        enumerator_->Release();
        CoUninitialize();
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

    CoUninitialize();
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
        props->GetValue(PKEY_Device_FriendlyName, &varName);

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
