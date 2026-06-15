#ifndef _COREAUDIO_INPUT_H_
#define _COREAUDIO_INPUT_H_

#include "audio/audio_input.h"

#include <AudioToolbox/AudioToolbox.h>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <vector>

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

    // Called from the input render callback to buffer audio
    void bufferAudioData(const float *audioData, std::size_t frameCount);

private:
    AudioInputConfig config_;
    std::string lastError_;

    AudioComponentInstance audioUnit_ = nullptr;
    bool isCapturing_ = false;

    // Circular buffer for captured audio
    std::vector<float> circularBuffer_;
    std::atomic<std::size_t> writePos_{0};
    std::atomic<std::size_t> readPos_{0};
    std::mutex bufferMutex_;
    std::condition_variable bufferCV_;
};

}  // namespace audio

#endif
