// win_multi_input — the Windows INPUT dispatcher: one device list, two
// backends behind it. Proposal 35, Phase 3. The twin of win_multi_backend.h,
// and everything said there applies here.
//
// MIXED MODE WORKS BY CONSTRUCTION, and that is the payoff of routing the two
// directions independently: WASAPI out + ASIO in, or ASIO out + WASAPI in, are
// just two device ids that happen to resolve to different worlds. Nothing
// coordinates them because nothing has to.
//
// What is NOT free is ASIO-driver-A out with ASIO-driver-B in: the callbacks
// have one process-global slot, so `AsioDeviceRegistry` refuses the second
// driver with a message rather than letting two of them fight over the
// trampolines. That refusal surfaces here as an open failure with a reason.

#pragma once

#include "tw/devices/audio_input.h"

#include <memory>
#include <mutex>
#include <string>

namespace audio {

class WinMultiInput : public AudioInput {
public:
    WinMultiInput();
    ~WinMultiInput() override;

    int openDevice(const std::string &deviceId = "default",
                   std::uint32_t preferredRate = 0) override;
    int closeDevice() override;
    int startCapture() override;
    int stopCapture() override;
    std::int32_t read(float *interleaved, std::size_t frameCount) override;

    int requestChannels(std::uint64_t mask) override;

    const AudioInputConfig &getConfig() const override;
    AudioInputStats stats() const override;
    std::vector<AudioInputDeviceInfo> listDevices() const override;
    std::vector<std::uint32_t> getAvailableBufferSizes() const override;
    int setBufferSize(std::uint32_t frameCount) override;
    const char *errorMessage() const override;

    // Reports the ACTIVE backend, not "winmulti" — a log line naming which of
    // the two is carrying the audio is what people need when a device
    // misbehaves.
    const char *backendName() const override;

private:
    AudioInput *active_() const;

    mutable std::mutex          mutex_;
    std::unique_ptr<AudioInput> wasapi_;
    std::unique_ptr<AudioInput> asio_;
    AudioInput                 *current_ = nullptr;
    std::uint64_t               pendingMask_ = 0;
    std::string                 error_;
};

}  // namespace audio
