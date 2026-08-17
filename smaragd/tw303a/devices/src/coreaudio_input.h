#ifndef _COREAUDIO_INPUT_H_
#define _COREAUDIO_INPUT_H_

#include "tw/devices/audio_input.h"
#include "tw/devices/audio_ring.h"

#include <AudioToolbox/AudioToolbox.h>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
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
    AudioInputStats stats() const override;
    std::vector<AudioInputDeviceInfo> listDevices() const override;
    const char *errorMessage() const override;
    const char *backendName() const override { return "coreaudio"; }

    // Called from the input render callback to buffer audio
    void bufferAudioData(const float *audioData, std::size_t frameCount);

    // Called from AVAudioEngine tap (Objective-C++, takes AVAudioPCMBuffer*)
    void captureAVAudioBuffer(void *avAudioPCMBuffer);

private:
    AudioInputConfig config_;
    std::string lastError_;

    AudioComponentInstance audioUnit_ = nullptr;
    bool isCapturing_ = false;
    std::atomic<bool> stopCaptureThread_{false};
    std::unique_ptr<std::thread> captureThread_;

    // Proposal 21 L0: the AVAudioEngine tap IS the capture thread — it is
    // already an event-driven producer on a thread we do not own — so this
    // backend needed no thread of its own, only the shared ring in place of the
    // hand-rolled circular buffer + mutex + condition_variable it had. read()
    // is now a non-blocking pop, as the interface always said it was: the old
    // one waited up to 100 ms on the CV, which turned a poll into a block.
    // UNVERIFIED: written and reviewed on Windows, compiled and run nowhere in
    // the L0 gate.
    AudioRing ring_;
    std::atomic<std::uint64_t> wakeups_{0};
};

}  // namespace audio

#endif
