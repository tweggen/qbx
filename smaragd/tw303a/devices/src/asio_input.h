// asio_input — the AudioInput facade over the SHARED AsioDevice.
// Proposal 35, Phase 3.
//
// PRIVATE header (devices/src/), like wasapi_input.h.
//
// THIS IS WHERE FULL DUPLEX ACTUALLY HAPPENS, and it is why Phase 2 built the
// device as a shared, refcounted object rather than folding it into the
// backend. `AsioBackend` and `AsioInput` acquire the SAME `AsioDevice` from
// the registry, so:
//
//   - one `IASIO` instance, one `createBuffers`, one callback, one clock —
//     which is the entire point of ASIO over WASAPI for recording, and the fix
//     for the endpoint sample-rate trap (CLAUDE.md): capture and render cannot
//     drift because there is only one clock to drift from;
//   - starting a recording while ASIO playback runs does NOT restart the
//     driver — `startRef`/`stopRef` are counted, so the first caller starts the
//     stream and the last one stops it;
//   - recording alone starts the stream with the output half emitting silence,
//     because no render callback is attached.
//
// The device is what refuses a SECOND ASIO driver (the callbacks have one
// global slot), so "driver A out + driver B in" is answered here with a
// readable error rather than by two drivers fighting over the trampolines.

#pragma once

#include "tw/devices/audio_input.h"

#include "asio_device.h"

#include <memory>
#include <mutex>
#include <string>

namespace audio {

class AsioInput : public AudioInput {
public:
    AsioInput() = default;
    ~AsioInput() override;

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
    const char *errorMessage() const override;
    const char *backendName() const override { return "asio"; }

private:
    void refreshConfig_();  // caller holds mutex_

    mutable std::mutex          mutex_;
    std::shared_ptr<AsioDevice> device_;
    AudioInputConfig            config_;
    bool                        startedHere_ = false;
    std::uint64_t               pendingMask_ = 0;  // requested before openDevice
    std::string                 error_;
};

}  // namespace audio
