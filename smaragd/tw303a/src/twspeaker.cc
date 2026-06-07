#include "twspeaker.h"

#include "audio/audio_backend.h"
#include "sapplication.h"
#include "twnegotiator.h"
#include "twsyslog.h"

#include <algorithm>
#include <cassert>
#include <cstdio>

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

    fprintf(stderr, "twSpeaker::startOutput: ENTER - backend=%p, outputDeviceId=%s\n",
           backend_.get(), outputDeviceId_.c_str());
    fflush(stderr);

    // The graph (synth) runs at its input wire's rate — the project/env rate.
    // Request that rate from the device so that, when the device can open there,
    // no resampling is needed at all.
    std::uint32_t graphRate = (std::uint32_t) env.getSRate();
    if (pInputPlugs != nullptr && pInputPlugs[0] != nullptr) {
        graphRate = pInputPlugs[0]->getFormat().sampleRate;
    }

    fprintf(stderr, "twSpeaker::startOutput: calling openDevice with rate=%u\n", graphRate);
    if (backend_->openDevice(outputDeviceId_, graphRate) != 0) {
        fprintf(stderr, "twSpeaker::startOutput: openDevice FAILED\n");
        return;
    }
    fprintf(stderr, "twSpeaker::startOutput: openDevice succeeded\n");

    // Negotiate one rate per wire across the graph feeding this speaker, folding
    // in the rates the device advertises so a device-native rate can win.
    // Advisory: logged, playback proceeds regardless — the resampler below
    // bridges any residual mismatch.
    {
        twNegotiator negotiator(env);
        negotiator.negotiate(this, backend_->supportedRates());
    }

    // Reconcile the graph rate with the rate the device actually opened at. The
    // resampler is a passthrough when the device honoured the request.
    const audio::AudioConfig cfg = backend_->getConfig();
    resampler_.configure(graphRate, cfg.sampleRate);
    resampler_.reserveHint((length_t) cfg.bufferFrames);
    resampler_.reset();

    // Sample-rate diagnostic (pitch/too-fast bug). Three numbers pin down where a
    // mismatch hides: the project rate, what the input wire claims to produce, and
    // what the device actually opened at. If wire == device but a 44.1 kHz sample
    // still plays fast, the source/reader isn't resampling file-rate -> project-rate
    // (the resampler here is correctly a passthrough). If wire != device but
    // passthrough is true, the boundary resampler failed to engage.
    {
        std::uint32_t wireRate = (pInputPlugs != nullptr && pInputPlugs[0] != nullptr)
                                     ? pInputPlugs[0]->getFormat().sampleRate
                                     : 0;
        syslog(LOG_INFO,
               "twSpeaker: rate diag — project(env)=%d Hz, wire=%u Hz, "
               "device=%u Hz, resampler=%s",
               env.getSRate(), (unsigned) wireRate, (unsigned) cfg.sampleRate,
               resampler_.isPassthrough() ? "passthrough" : "active");
    }

    if (!resampler_.isPassthrough()) {
        syslog(LOG_INFO, "twSpeaker: resampling %u Hz -> %u Hz",
               (unsigned) graphRate, (unsigned) cfg.sampleRate);
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

    fprintf(stderr, "twSpeaker::startOutput: calling backend->startOutput()\n");
    if (backend_->startOutput() != 0) {
        fprintf(stderr, "twSpeaker::startOutput: backend startOutput FAILED\n");
        backend_->closeDevice();
        return;
    }
    fprintf(stderr, "twSpeaker::startOutput: backend->startOutput() succeeded\n");
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

void twSpeaker::setOutputDevice(const std::string &id)
{
    outputDeviceId_ = id.empty() ? "default" : id;
}

std::vector<audio::AudioDeviceInfo> twSpeaker::outputDevices() const
{
    return backend_->enumerateDevices();
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
