#ifndef _AUDIO_ALSA_BACKEND_H_
#define _AUDIO_ALSA_BACKEND_H_

#include "audio/audio_backend.h"

#define ALSA_PCM_NEW_HW_PARAMS_API
#include <alsa/asoundlib.h>

#include <vector>

namespace audio {

class ALSABackend : public AudioBackend {
public:
    ALSABackend();
    ~ALSABackend() override;

    int  openDevice(const std::string &deviceName = "default",
                    std::uint32_t preferredRate = 0) override;
    int  closeDevice() override;
    int  startOutput() override;
    int  stopOutput() override;
    bool isRunning() const override { return running_; }

    void setRenderCallback(RenderCallback cb) override { callback_ = std::move(cb); }
    AudioConfig getConfig() const override { return config_; }

    // Rates the PCM device accepts, probed from the candidate set.
    std::vector<std::uint32_t> supportedRates() const override;
    // Enumerate ALSA cards (snd_card_next) as selectable devices.
    std::vector<AudioDeviceInfo> enumerateDevices() const override;

    const char *name() const override { return "alsa"; }

private:
    static void asyncCallbackStatic_(snd_async_handler_t *handler);
    void        asyncCallback_();
    void        writeChunk_(snd_pcm_uframes_t chunkSize);
    std::size_t pullSamples_();

    snd_pcm_t           *pcm_           = nullptr;
    snd_async_handler_t *asyncHandle_   = nullptr;

    AudioConfig          config_;
    RenderCallback       callback_;

    std::vector<float>   floatBuffer_;  // sized to bufferFrames * channels
    std::vector<int16_t> shortBuffer_;  // sized to bufferFrames * channels

    bool                 running_ = false;
};

}  // namespace audio

#endif
