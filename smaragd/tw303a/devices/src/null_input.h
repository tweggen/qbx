#ifndef _NULL_INPUT_H_
#define _NULL_INPUT_H_

#include <atomic>
#include <cstdint>
#include "tw/devices/audio_input.h"

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
    const char *backendName() const override { return "null"; }

private:
    AudioInputConfig config_;
    std::string lastError_;
    // PACING (proposal 21 integration fix): a null input is a SILENT DEVICE, not an
    // infinite source. read() hands out only the frames real time has produced
    // since startCapture() at config_.sampleRate, so a consumer that loops on
    // "got == 0 -> idle" (CaptureBridge) idles and can be stopped, and a growing
    // recording grows at real time instead of as fast as memset can run.
    std::atomic<bool> capturing_{ false };
    std::int64_t startNs_ = 0;
    std::uint64_t producedFrames_ = 0;
};

}  // namespace audio

#endif
