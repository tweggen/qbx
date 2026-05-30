#ifndef _AUDIO_BACKEND_H_
#define _AUDIO_BACKEND_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace audio {

struct AudioConfig {
    uint32_t sampleRate   = 44100;
    uint32_t channels     = 2;
    uint32_t bufferFrames = 1024;
    uint32_t periodFrames = 64;
};

// Pull callback: fill `out` with up to `frames` interleaved float samples
// across `channels` channels. Return the number of frames actually written
// (may be less than requested when the source is dry; backends should treat
// shortfalls as silence, not as an error).
using RenderCallback =
    std::function<std::size_t(float *out, std::size_t frames, std::uint32_t channels)>;

class AudioBackend {
public:
    virtual ~AudioBackend() = default;

    virtual int  openDevice(const std::string &deviceName = "default") = 0;
    virtual int  closeDevice()                                          = 0;

    virtual int  startOutput()                                          = 0;
    virtual int  stopOutput()                                           = 0;
    virtual bool isRunning() const                                      = 0;

    virtual void setRenderCallback(RenderCallback cb)                   = 0;
    virtual AudioConfig getConfig() const                               = 0;

    virtual const char *name() const                                    = 0;
};

std::unique_ptr<AudioBackend> createAudioBackend();

}  // namespace audio

#endif
