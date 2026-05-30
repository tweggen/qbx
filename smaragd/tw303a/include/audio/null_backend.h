#ifndef _AUDIO_NULL_BACKEND_H_
#define _AUDIO_NULL_BACKEND_H_

#include "audio/audio_backend.h"

namespace audio {

// Drop-in backend for platforms with no working audio output. Honours the
// AudioBackend lifecycle but produces no sound. Lets the rest of the app
// (UI, project loading, rendering to file) run on Windows/macOS during the
// transition while WASAPI/CoreAudio backends are being written.
class NullBackend : public AudioBackend {
public:
    NullBackend();
    ~NullBackend() override;

    int  openDevice(const std::string &deviceName = "default") override;
    int  closeDevice() override;
    int  startOutput() override;
    int  stopOutput() override;
    bool isRunning() const override { return running_; }

    void setRenderCallback(RenderCallback cb) override { callback_ = std::move(cb); }
    AudioConfig getConfig() const override { return config_; }

    const char *name() const override { return "null"; }

private:
    AudioConfig    config_;
    RenderCallback callback_;
    bool           running_ = false;
};

}  // namespace audio

#endif
