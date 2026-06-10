#ifndef _COREAUDIO_INPUT_H_
#define _COREAUDIO_INPUT_H_

#include "audio/audio_input.h"

#include <CoreAudio/CoreAudio.h>
#include <AudioToolbox/AudioToolbox.h>

namespace audio {

class CoreAudioInput : public AudioInput {
public:
    CoreAudioInput();
    ~CoreAudioInput() override;

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

    AudioDeviceID inputDeviceID_ = 0;
    AudioComponentInstance audioUnit_ = nullptr;
    bool isCapturing_ = false;
};

}  // namespace audio

#endif
