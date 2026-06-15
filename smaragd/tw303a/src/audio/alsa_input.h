#ifndef _ALSA_INPUT_H_
#define _ALSA_INPUT_H_

#include "audio/audio_input.h"

#include <alsa/asoundlib.h>

namespace audio {

class ALSAInput : public AudioInput {
public:
    ALSAInput();
    ~ALSAInput() override;

    int openDevice(const std::string &deviceId = "default",
                   std::uint32_t preferredRate = 0) override;
    int closeDevice() override;
    int startCapture() override;
    int stopCapture() override;
    std::int32_t read(float *interleaved, std::size_t frameCount) override;

    const AudioInputConfig &getConfig() const override;
    std::vector<AudioInputDeviceInfo> listDevices() const override;
    const char *errorMessage() const override;

    // ALSA supports user-selectable buffer sizes. Returns list of common presets.
    std::vector<uint32_t> getAvailableBufferSizes() const override;
    // Change buffer size (must be stopped). Reconfigures hw params.
    int setBufferSize(uint32_t frameCount) override;

private:
    AudioInputConfig config_;
    std::string lastError_;
    snd_pcm_t *pcmHandle_ = nullptr;
    bool isCapturing_ = false;
};

}  // namespace audio

#endif
