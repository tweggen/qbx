#include "tw/devices/null_backend.h"

#include "tw/core/twsyslog.h"

namespace audio {

NullBackend::NullBackend()  = default;
NullBackend::~NullBackend() = default;

int NullBackend::openDevice(const std::string & /*deviceName*/,
                            std::uint32_t preferredRate)
{
    // No hardware to constrain us: adopt the requested rate so the speaker
    // resampler stays a passthrough. 0 keeps the default.
    if (preferredRate != 0) config_.sampleRate = preferredRate;

    // Set a reasonable default latency estimate (one buffer frame).
    config_.outputLatencyFrames = config_.bufferFrames;

    syslog(LOG_INFO,
           "audio: NullBackend active (%u Hz) — no sound will be produced.",
           (unsigned) config_.sampleRate);
    return 0;
}

int NullBackend::closeDevice()
{
    return 0;
}

int NullBackend::startOutput()
{
    running_ = true;
    return 0;
}

int NullBackend::stopOutput()
{
    running_ = false;
    return 0;
}

}  // namespace audio
