#ifndef _NULL_INPUT_H_
#define _NULL_INPUT_H_

#include "audio/audio_input.h"

namespace audio {

class NullInput : public AudioInput {
public:
    NullInput();
    ~NullInput() override;

    int openDevice(const std::string &deviceId = "default",
                   std::uint32_t preferredRate = 0) override;
    int closeDevice() override;
    int startCapture() override;
    int stopCapture() override;
    std::int32_t read(float *interleaved, std::size_t frameCount) override;

    const AudioInputConfig &getConfig() const override;
    std::vector<AudioInputDeviceInfo> listDevices() const override;
    const char *errorMessage() const override;

private:
    AudioInputConfig config_;
    std::string lastError_;
};

}  // namespace audio

#endif
