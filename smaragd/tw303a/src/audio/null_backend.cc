#include "audio/null_backend.h"

#include "twsyslog.h"

namespace audio {

NullBackend::NullBackend()  = default;
NullBackend::~NullBackend() = default;

int NullBackend::openDevice(const std::string & /*deviceName*/)
{
    syslog(LOG_INFO, "audio: NullBackend active — no sound will be produced.");
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
