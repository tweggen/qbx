#ifndef _AUDIO_WASAPI_BACKEND_H_
#define _AUDIO_WASAPI_BACKEND_H_

#include "audio/audio_backend.h"

#include <atomic>
#include <mutex>
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

    int  openDevice(const std::string &deviceName = "default",
                    std::uint32_t preferredRate = 0) override;
    int  closeDevice() override;
    int  startOutput() override;
    int  stopOutput() override;
    bool isRunning() const override { return running_.load(); }

    void setRenderCallback(RenderCallback cb) override { callback_ = std::move(cb); }
    AudioConfig getConfig() const override { return config_; }

    // Shared mode: the OS mixer owns the rate, so this is just the current mix
    // rate (known after openDevice; empty before). Exclusive-mode enumeration
    // is future work — see proposal 02.
    std::vector<std::uint32_t> supportedRates() const override;
    std::vector<AudioDeviceInfo> enumerateDevices() const override;

    const char *name() const override { return "wasapi"; }

private:
    void audioThreadProc_();
    int  renderOnce_();
    // Release all device/COM handles. Caller MUST hold stateMutex_ (so openDevice
    // can reuse it while resetting a half-open state without re-locking). The
    // public closeDevice() is the locked wrapper.
    void releaseDevice_();

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
    // Serialises the lifecycle transitions (openDevice / closeDevice /
    // startOutput / stopOutput). The atomics above only protect individual flag
    // reads; this guards the compound open/close/start/stop logic so concurrent
    // callers can't interleave (e.g. play and record-monitoring both touching the
    // device). The audio thread never takes this lock, so holding it across
    // thread_.join() in stopOutput() cannot deadlock.
    std::mutex           stateMutex_;

    bool                 comInitialized_ = false;
};

}  // namespace audio

#endif
