// win_multi_backend — the Windows output dispatcher: one device list, two
// backends behind it. Proposal 35, Phase 2.
//
// PRIVATE header (devices/src/). `createAudioBackend()` returns one of these
// on Windows, so nothing above the module changes: `twSpeaker` still builds a
// backend once in its constructor and switching between WASAPI and ASIO is a
// plain device-id change, honouring the existing "takes effect on next Play"
// model.
//
// COMPILED WHENEVER QBX_WIN_WASAPI, with the ASIO halves under TW_HAVE_ASIO.
// That is deliberate: a build without the SDK still has to UNDERSTAND a
// prefixed id, or a user who selected an ASIO driver in an SDK-enabled build
// and then ran an SDK-less one would get a device id nothing could parse. In
// that build an `asio:` id resolves to no backend and says so, which is a
// readable failure rather than a mystery.
//
// ROUTING is `asio_id.h`'s and nothing else's: bare and legacy ids go to
// WASAPI, which is what keeps every persisted `audio/deviceId` working.

#pragma once

#include "tw/devices/audio_backend.h"

#include <memory>
#include <mutex>
#include <string>

namespace audio {

class WinMultiBackend : public AudioBackend {
public:
    WinMultiBackend();
    ~WinMultiBackend() override;

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
    std::uint32_t getLatencyFrames() const override;

    // Reports the ACTIVE backend, not "winmulti": callers log it, and a log
    // line saying which of the two is actually carrying the audio is the one
    // people need when a device misbehaves.
    const char *name() const override;

private:
    // The backend a device id routes to, created on demand. Only one is ever
    // open at a time — this is an output device, and the app plays through
    // exactly one.
    AudioBackend       *active_() const;

    mutable std::mutex             mutex_;
    std::unique_ptr<AudioBackend>  wasapi_;
    std::unique_ptr<AudioBackend>  asio_;
    AudioBackend                  *current_ = nullptr;
    RenderCallback                 callback_;
};

}  // namespace audio
