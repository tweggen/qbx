// asio_backend — the AudioBackend facade over the shared AsioDevice.
// Proposal 35, Phase 2.
//
// PRIVATE header (devices/src/, like wasapi_input.h): nothing outside the
// module names this type. The app reaches it through `createAudioBackend()`,
// which on Windows returns the `WinMultiBackend` dispatcher, which routes an
// `asio:` device id here.
//
// This class is deliberately THIN. Everything that touches the driver lives in
// AsioDevice, because the input facade (Phase 3) has to share exactly that
// state — one COM instance, one buffer set, one start refcount. What is here
// is the AudioBackend contract and nothing else.

#pragma once

#include "tw/devices/audio_backend.h"

#include "asio_device.h"

#include <memory>
#include <mutex>
#include <string>

namespace audio {

class AsioBackend : public AudioBackend {
public:
    AsioBackend() = default;
    ~AsioBackend() override;

    int  openDevice(const std::string &deviceName = "default",
                    std::uint32_t preferredRate = 0) override;
    int  closeDevice() override;
    int  startOutput() override;
    int  stopOutput() override;
    bool isRunning() const override;

    void setRenderCallback(RenderCallback cb) override;
    AudioConfig getConfig() const override;

    std::vector<std::uint32_t> supportedRates() const override;
    std::vector<AudioDeviceInfo> enumerateDevices() const override;
    std::vector<std::uint32_t> getAvailableBufferSizes() const override;
    int setBufferSize(std::uint32_t frameCount) override;

    const char *name() const override { return "asio"; }

    // Why the last openDevice failed. The dispatcher surfaces this so a user
    // who picked a driver another app is holding gets a reason rather than
    // silence.
    const std::string &errorMessage() const { return error_; }

private:
    mutable std::mutex           mutex_;
    std::shared_ptr<AsioDevice>  device_;
    RenderCallback               pendingCallback_;
    bool                         startedHere_ = false;
    std::string                  error_;
};

}  // namespace audio
