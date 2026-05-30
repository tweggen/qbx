#include "twspeaker.h"

#include "audio/audio_backend.h"
#include "sapplication.h"
#include "twsyslog.h"

#include <algorithm>

twSpeaker::twSpeaker(tw303aEnvironment &env0)
    : twComponent(env0),
      backend_(audio::createAudioBackend()),
      isPlaying_(false)
{
    syslog(LOG_INFO, "twSpeaker: using audio backend '%s'", backend_->name());
}

twSpeaker::~twSpeaker()
{
    if (isPlaying_) backend_->stopOutput();
    backend_->closeDevice();
}

void twSpeaker::startOutput()
{
    if (isPlaying_) return;

    if (backend_->openDevice("default") != 0) {
        syslog(LOG_ERR, "twSpeaker::startOutput: openDevice failed");
        return;
    }

    backend_->setRenderCallback(
        [this](float *out, std::size_t frames, std::uint32_t channels) -> std::size_t {
            if (pInputPlugs == nullptr || pInputPlugs[0] == nullptr) {
                std::fill_n(out, frames * channels, 0.0f);
                return frames;
            }

            offset_t formerPos = SApplication::app().getGlobalLocatorPos();
            length_t framesRead =
                static_cast<twLatchStreamingOutput *>(pInputPlugs[0])
                    ->readStreamingData(out, static_cast<length_t>(frames));

            if (framesRead <= 0) {
                std::fill_n(out, frames * channels, 0.0f);
                return frames;
            }

            // Mono synthesizer → fan out to N channels. We read mono into the
            // start of the buffer and expand in place from the tail.
            if (channels > 1) {
                for (length_t i = framesRead - 1; i >= 0; --i) {
                    float s = out[i];
                    for (std::uint32_t c = 0; c < channels; ++c) {
                        out[i * channels + c] = s;
                    }
                }
            }

            SApplication::app().setGlobalLocatorPos(formerPos + framesRead);
            return static_cast<std::size_t>(framesRead);
        });

    if (backend_->startOutput() != 0) {
        syslog(LOG_ERR, "twSpeaker::startOutput: backend startOutput failed");
        backend_->closeDevice();
        return;
    }
    isPlaying_ = true;
}

void twSpeaker::stopOutput()
{
    if (!isPlaying_) return;
    backend_->stopOutput();
    backend_->closeDevice();
    isPlaying_ = false;
}

bool twSpeaker::isPlaying()
{
    return isPlaying_;
}

length_t twSpeaker::calcOutputTo(sample_t *, length_t, idx_t)
{
    return 0;
}

void twSpeaker::createOutputLatches()
{
#ifdef DEBUG_COMPONENT
    syslog(LOG_DEBUG, "twSpeaker::createOutputLatches(): entered.");
#endif
}
