#include "twspeaker.h"

#include "audio/audio_backend.h"
#include "sapplication.h"
#include "twnegotiator.h"
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

    // Negotiate one format per wire across the graph feeding this speaker,
    // before opening the device. Advisory: the result is logged and the engine
    // proceeds regardless, because the speaker's own resampler bridges the
    // graph rate to the device rate in all cases.
    {
        twNegotiator negotiator(env);
        negotiator.negotiate(this);
    }

    if (backend_->openDevice("default") != 0) {
        syslog(LOG_ERR, "twSpeaker::startOutput: openDevice failed");
        return;
    }

    // Reconcile the synth's sample rate with the device's. The device mix rate
    // is whatever the backend negotiated (commonly 48 kHz); the synth produces
    // at its input wire's rate (commonly 44.1 kHz). The resampler bridges them;
    // it is a passthrough when they already match.
    const audio::AudioConfig cfg = backend_->getConfig();
    std::uint32_t inRate = (std::uint32_t) env.getSRate();
    if (pInputPlugs != nullptr && pInputPlugs[0] != nullptr) {
        inRate = pInputPlugs[0]->getFormat().sampleRate;
    }
    resampler_.configure(inRate, cfg.sampleRate);
    resampler_.reserveHint((length_t) cfg.bufferFrames);
    resampler_.reset();
    if (!resampler_.isPassthrough()) {
        syslog(LOG_INFO, "twSpeaker: resampling %u Hz -> %u Hz",
               (unsigned) inRate, (unsigned) cfg.sampleRate);
    }

    backend_->setRenderCallback(
        [this](float *out, std::size_t frames, std::uint32_t channels) -> std::size_t {
            if (pInputPlugs == nullptr || pInputPlugs[0] == nullptr) {
                std::fill_n(out, frames * channels, 0.0f);
                return frames;
            }

            offset_t formerPos = SApplication::app().getGlobalLocatorPos();
            length_t inConsumed = 0;
            length_t framesOut = resampler_.process(
                static_cast<twLatchStreamingOutput *>(pInputPlugs[0]),
                out, static_cast<length_t>(frames), &inConsumed);

            if (framesOut <= 0) {
                std::fill_n(out, frames * channels, 0.0f);
                return frames;
            }

            // Mono synthesizer → fan out to N channels. The resampler wrote mono
            // into the start of the buffer; expand in place from the tail.
            if (channels > 1) {
                for (length_t i = framesOut - 1; i >= 0; --i) {
                    float s = out[i];
                    for (std::uint32_t c = 0; c < channels; ++c) {
                        out[i * channels + c] = s;
                    }
                }
            }

            // Advance the playback locator by INPUT frames consumed (synth-time),
            // not output frames — the two differ once resampling is active.
            SApplication::app().setGlobalLocatorPos(formerPos + inConsumed);
            return static_cast<std::size_t>(framesOut);
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
