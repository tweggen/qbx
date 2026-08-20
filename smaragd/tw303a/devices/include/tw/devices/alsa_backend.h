#ifndef _AUDIO_ALSA_BACKEND_H_
#define _AUDIO_ALSA_BACKEND_H_

#include "tw/devices/audio_backend.h"

#define ALSA_PCM_NEW_HW_PARAMS_API
#include <alsa/asoundlib.h>

#include <atomic>
#include <thread>
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

    // ALSA supports user-selectable buffer sizes. Returns list of common presets.
    std::vector<uint32_t> getAvailableBufferSizes() const override;
    // Change buffer size (must be stopped). Reconfigures hw params.
    int setBufferSize(uint32_t frameCount) override;

    const char *name() const override { return "alsa"; }

private:
    // Blocking-write I/O pump, run on its own thread while running_. See the
    // long comment on the .cc definition for why this replaced the earlier
    // snd_async_add_pcm_handler design.
    void        ioThreadFunc_();
    // Pulls exactly `framesWanted` frames from the render callback into
    // floatBuffer_ (silence-padded on a short return). Never pulls more than
    // it is about to hand to the device -- see ioThreadFunc_.
    std::size_t pullSamples_(std::size_t framesWanted);
    // Converts the first `frameCount` frames of floatBuffer_ and writes them
    // to the device, retrying on a partial write / recovering on an xrun,
    // until every frame is written (or an unrecoverable error occurs).
    void        writeChunk_(std::size_t frameCount);

    snd_pcm_t           *pcm_           = nullptr;

    std::thread          ioThread_;
    std::atomic<bool>    stopRequested_{false};

    AudioConfig          config_;
    RenderCallback       callback_;

    std::vector<float>   floatBuffer_;  // sized to bufferFrames * channels
    std::vector<int16_t> shortBuffer_;  // sized to bufferFrames * channels

    bool                 running_ = false;
};

}  // namespace audio

#endif
