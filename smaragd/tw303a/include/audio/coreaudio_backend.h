#ifndef _AUDIO_COREAUDIO_BACKEND_H_
#define _AUDIO_COREAUDIO_BACKEND_H_

#include "audio/audio_backend.h"

#include <atomic>
#include <vector>

namespace audio {

// Opaque type declarations (defined only in the .cc when CoreAudio headers are included)
typedef void *AudioUnit;

class CoreAudioBackend : public AudioBackend {
public:
    CoreAudioBackend();
    ~CoreAudioBackend() override;

    int  openDevice(const std::string &deviceId = "default",
                    std::uint32_t preferredRate = 0) override;
    int  closeDevice() override;
    int  startOutput() override;
    int  stopOutput() override;
    bool isRunning() const override { return running_.load(); }

    void setRenderCallback(RenderCallback cb) override { callback_ = std::move(cb); }
    AudioConfig getConfig() const override { return config_; }

    // Return the device's mix rate once opened. Can enumerate if multiple output
    // devices are available via Audio Hardware APIs.
    std::vector<std::uint32_t> supportedRates() const override;
    std::vector<AudioDeviceInfo> enumerateDevices() const override;

    const char *name() const override { return "coreaudio"; }

private:
    // CoreAudio render callback (static, dispatches to instance).
    // Signature matches AURenderCallback; refCon is the CoreAudioBackend instance.
    // Return: 0 (noErr) on success. Parameters are forward-declared as void* to
    // avoid pulling CoreAudio headers into the header file.
    static int renderCallback_(void *refCon,
                               void *flags,  // AudioUnitRenderActionFlags*
                               const void *ts,  // AudioTimeStamp*
                               unsigned int busNum,
                               unsigned int frames,
                               void *buffers);  // AudioBufferList*

    void renderOnce_(unsigned int frames, void *buffers);

    AudioUnit            outputUnit_  = nullptr;
    unsigned int         deviceId_    = 0;  // kAudioObjectSystemObject or a specific device
    AudioConfig          config_;
    RenderCallback       callback_;
    std::vector<float>   floatScratch_;

    std::atomic<bool>    running_{false};
};

}  // namespace audio

#endif
