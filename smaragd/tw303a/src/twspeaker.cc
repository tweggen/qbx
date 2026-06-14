#include "twspeaker.h"

#include "audio/audio_backend.h"
#include "sapplication.h"
#include "sproject.h"
#include "sobject.h"
#include "twnegotiator.h"
#include "twsyslog.h"

#include <algorithm>
#include <cassert>
#include <cmath>
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

    // Output-to-input frame ratio, used to bound a pull at the loop end during
    // cycle playback. 1.0 when the resampler is a passthrough.
    rateRatio_ = (graphRate > 0) ? ((double) cfg.sampleRate / (double) graphRate) : 1.0;

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

            auto *input = static_cast<twLatchStreamingOutput *>(pInputPlugs[0]);
            offset_t pos = SApplication::app().getGlobalLocatorPos();

            const bool     cycle     = cycleEnabled_.load(std::memory_order_relaxed);
            const offset_t loopStart = loopStart_.load(std::memory_order_relaxed);
            const offset_t loopEnd   = loopEnd_.load(std::memory_order_relaxed);
            const bool     loopValid = cycle && loopEnd > loopStart;

            // Fill the buffer, wrapping back to loopStart whenever the cursor
            // reaches loopEnd. Without cycling this runs exactly one pull, which
            // is the original straight-through behaviour. Mono is written
            // contiguously into out[0..]; the fan-out to N channels happens once
            // the whole buffer is filled.
            std::size_t filled = 0;
            while (filled < frames) {
                if (loopValid && pos >= loopEnd) {
                    // Seamless wrap: re-seek the graph to the loop start. (seekTo
                    // only resets cursor positions — cheap and lock-free, and we
                    // are the only thread pulling.)
                    if (SProject *proj = SApplication::app().getCurrentProject()) {
                        if (SObject *root = proj->getRootComponent())
                            root->seekTo(loopStart);
                    }
                    pos = loopStart;
                }

                length_t want = static_cast<length_t>(frames - filled);
                if (loopValid) {
                    // Don't pull past the loop end in this chunk: bound the
                    // request by the output frames that fit in the remaining
                    // input (synth-time) frames before loopEnd.
                    double outLeft = (double)(loopEnd - pos) * rateRatio_;
                    length_t cap = (length_t) std::llround(outLeft);
                    if (cap < 1) cap = 1;            // always make progress
                    if (want > cap) want = cap;
                }

                length_t inConsumed = 0;
                length_t got = resampler_.process(input, out + filled, want, &inConsumed);
                if (got <= 0) break;                 // source dry — stop filling

                pos += (offset_t) inConsumed;
                filled += (std::size_t) got;

                if (!loopValid) break;               // single pull when not cycling
            }

            if (filled == 0) {
                std::fill_n(out, frames * channels, 0.0f);
                // Realtime store only — never emit Qt signals from this thread.
                // While recording, the record worker owns the locator (it tracks
                // captured frames), so monitoring playback must not move it.
                if (!SApplication::app().isRecordingActive())
                    SApplication::app().setGlobalLocatorPosRealtime(pos);
                return frames;
            }

            // Pad any unfilled tail with silence so the whole buffer is defined
            // (only happens when the source ran dry mid-buffer).
            if (filled < frames)
                std::fill(out + filled, out + frames, 0.0f);

            std::size_t outFrames = loopValid ? frames : filled;

            // Mono synthesizer → fan out to N channels. The mono samples sit at
            // the start of the buffer; expand in place from the tail.
            if (channels > 1) {
                for (length_t i = (length_t) outFrames - 1; i >= 0; --i) {
                    float s = out[i];
                    for (std::uint32_t c = 0; c < channels; ++c) {
                        out[i * channels + c] = s;
                    }
                }
            }

            // Advance (or wrap) the playback locator. pos already reflects any
            // loop wraps that happened above. Realtime store only — never emit Qt
            // signals from this thread (that would adopt it and deadlock the
            // join() in stopOutput at thread teardown). While recording, the
            // record worker owns the locator, so monitoring playback must not
            // move it (it would fight the capture position).
            if (!SApplication::app().isRecordingActive())
                SApplication::app().setGlobalLocatorPosRealtime(pos);
            return static_cast<std::size_t>(outFrames);
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

void twSpeaker::setCycle(bool enabled, offset_t startFrame, offset_t endFrame)
{
    // An empty or inverted range can't loop; treat it as cycle-off.
    if (endFrame <= startFrame) enabled = false;
    loopStart_.store(startFrame, std::memory_order_relaxed);
    loopEnd_.store(endFrame, std::memory_order_relaxed);
    cycleEnabled_.store(enabled, std::memory_order_relaxed);
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
