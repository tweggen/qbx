#ifndef _AUDIO_WASAPI_BACKEND_H_
#define _AUDIO_WASAPI_BACKEND_H_

#include "audio/audio_backend.h"

#include <atomic>
#include <thread>
#include <vector>

// Forward-declare COM interfaces so this header doesn't pull in <windows.h>
// everywhere — keeps Qt MOC happy and stops the engine headers from leaking
// Windows macros (NOMINMAX et al.) into every translation unit.
struct IMMDeviceEnumerator;
struct IMMDevice;
struct IAudioClient;
struct IAudioRenderClient;
typedef void *HANDLE;

namespace audio {

enum class WasapiSampleFormat {
    Unknown,
    Float32,
    Int16,
    Int32,
};

class WASAPIBackend : public AudioBackend {
public:
    WASAPIBackend();
    ~WASAPIBackend() override;

    int  openDevice(const std::string &deviceName = "default") override;
    int  closeDevice() override;
    int  startOutput() override;
    int  stopOutput() override;
    bool isRunning() const override { return running_.load(); }

    void setRenderCallback(RenderCallback cb) override { callback_ = std::move(cb); }
    AudioConfig getConfig() const override { return config_; }

    const char *name() const override { return "wasapi"; }

private:
    void audioThreadProc_();
    int  renderOnce_();

    IMMDeviceEnumerator *enumerator_   = nullptr;
    IMMDevice           *device_       = nullptr;
    IAudioClient        *audioClient_  = nullptr;
    IAudioRenderClient  *renderClient_ = nullptr;
    HANDLE               bufferReady_  = nullptr;

    WasapiSampleFormat   sampleFormat_ = WasapiSampleFormat::Unknown;
    AudioConfig          config_;
    RenderCallback       callback_;

    std::vector<float>   floatScratch_;  // interleaved float pulled from the synth

    std::thread          thread_;
    std::atomic<bool>    running_{false};
    std::atomic<bool>    stopFlag_{false};

    bool                 comInitialized_ = false;
};

}  // namespace audio

#endif
