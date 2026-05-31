#ifndef _AUDIO_BACKEND_H_
#define _AUDIO_BACKEND_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "twformat.h"   // twSampleType

namespace audio {

struct AudioConfig {
    uint32_t     sampleRate   = 44100;
    uint32_t     channels     = 2;
    uint32_t     bufferFrames = 1024;
    uint32_t     periodFrames = 64;
    // The device's native binary sample format (mirrors twFormat.sampleType),
    // so the device boundary is just another wire and the converter targets it
    // directly. Reported via getConfig() after the device is opened.
    twSampleType sampleType   = twSampleType::Float32;
};

// One selectable output device. `id` is the backend-specific handle passed back
// to openDevice() (e.g. a WASAPI endpoint id); `name` is the human-readable
// label for the UI. The empty id / "default" means the system default endpoint.
struct AudioDeviceInfo {
    std::string id;
    std::string name;
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

    // Open a device, optionally requesting a preferred sample rate. The backend
    // opens at preferredRate iff it can support it natively, otherwise at its
    // default / mix rate; the rate (and sampleType) actually in force is
    // reported by getConfig(). preferredRate == 0 means "no preference".
    virtual int  openDevice(const std::string &deviceName = "default",
                            std::uint32_t preferredRate = 0)            = 0;
    virtual int  closeDevice()                                          = 0;

    virtual int  startOutput()                                          = 0;
    virtual int  stopOutput()                                           = 0;
    virtual bool isRunning() const                                      = 0;

    virtual void setRenderCallback(RenderCallback cb)                   = 0;
    virtual AudioConfig getConfig() const                               = 0;

    // Rates this device can be opened at WITHOUT the host resampling. A
    // shared-mode backend reports its single mix rate; an exclusive-capable one
    // may report several. Empty == unknown until opened. Pure query: it may
    // probe the device but must not disturb an active stream. Feeds the
    // negotiator's candidate domain D.
    virtual std::vector<std::uint32_t> supportedRates() const           = 0;

    // Enumerate the selectable output devices for a device-picker UI. The empty
    // list means "only the system default is available". May be called before
    // openDevice().
    virtual std::vector<AudioDeviceInfo> enumerateDevices() const        = 0;

    virtual const char *name() const                                    = 0;
};

std::unique_ptr<AudioBackend> createAudioBackend();

}  // namespace audio

#endif
