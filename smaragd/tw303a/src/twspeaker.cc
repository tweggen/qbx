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

// All diagnostic output in this file goes to stderr, tagged with the source file
// and function so each line is self-locating and greppable. Flushed immediately
// so the last message before a crash/hang is never lost in a buffer.
#define TWSPK_LOG( fmt, ... )                                                   \
    do {                                                                        \
        fprintf( stderr, "twspeaker.cc:%s: " fmt "\n", __func__, ##__VA_ARGS__ ); \
        fflush( stderr );                                                       \
    } while( 0 )

twSpeaker::twSpeaker(tw303aEnvironment &env0)
    : twComponent(env0),
      backend_(audio::createAudioBackend()),
      isPlaying_(false)
{
    TWSPK_LOG( "using audio backend '%s'", backend_->name() );
}

twSpeaker::~twSpeaker()
{
    TWSPK_LOG( "destroying (isPlaying=%d)", (int) isPlaying_.load() );
    if (isPlaying_) backend_->stopOutput();
    backend_->closeDevice();
}

void twSpeaker::startOutput()
{
    std::lock_guard<std::mutex> lock(outputMutex_);
    if (isPlaying_) return;

    TWSPK_LOG( "ENTER - backend=%p, outputDeviceId=%s",
               backend_.get(), outputDeviceId_.c_str() );

    // The graph (synth) runs at its input wire's rate — the project/env rate.
    // Request that rate from the device so that, when the device can open there,
    // no resampling is needed at all.
    std::uint32_t graphRate = (std::uint32_t) env.getSRate();
    if (pInputPlugs != nullptr && pInputPlugs[0] != nullptr) {
        graphRate = pInputPlugs[0]->getFormat().sampleRate;
    }

    TWSPK_LOG( "calling openDevice with rate=%u", graphRate );
    if (backend_->openDevice(outputDeviceId_, graphRate) != 0) {
        TWSPK_LOG( "openDevice FAILED" );
        return;
    }
    TWSPK_LOG( "openDevice succeeded" );

    // Negotiate one rate per wire across the graph feeding this speaker, folding
    // in the rates the device advertises so a device-native rate can win.
    // Advisory: logged, playback proceeds regardless — the resampler below
    // bridges any residual mismatch.
    {
        twNegotiator negotiator(env);
        negotiator.negotiate(this, backend_->supportedRates());
    }

    // Reconcile the graph rate with the rate the device actually opened at. The
    // resamplers are passthroughs when the device honoured the request.
    const audio::AudioConfig cfg = backend_->getConfig();
    resamplerL_.configure(graphRate, cfg.sampleRate);
    resamplerR_.configure(graphRate, cfg.sampleRate);
    resamplerL_.reserveHint((length_t) cfg.bufferFrames);
    resamplerR_.reserveHint((length_t) cfg.bufferFrames);
    resamplerL_.reset();
    resamplerR_.reset();

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
        TWSPK_LOG( "rate diag — project(env)=%d Hz, wire=%u Hz, "
                   "device=%u Hz, resampler=%s",
                   env.getSRate(), (unsigned) wireRate, (unsigned) cfg.sampleRate,
                   resamplerL_.isPassthrough() ? "passthrough" : "active" );
    }

    if (!resamplerL_.isPassthrough()) {
        TWSPK_LOG( "resampling %u Hz -> %u Hz",
                   (unsigned) graphRate, (unsigned) cfg.sampleRate );
    }

    backend_->setRenderCallback(
        [this](float *out, std::size_t frames, std::uint32_t channels) -> std::size_t {
            if (pInputPlugs == nullptr || pInputPlugs[0] == nullptr) {
                std::fill_n(out, frames * channels, 0.0f);
                return frames;
            }

            auto *inputL = static_cast<twLatchStreamingOutput *>(pInputPlugs[0]);
            auto *inputR = (pInputPlugs[1] != nullptr)
                             ? static_cast<twLatchStreamingOutput *>(pInputPlugs[1])
                             : nullptr;
            offset_t pos = SApplication::app().getGlobalLocatorPos();

            const bool     cycle     = cycleEnabled_.load(std::memory_order_relaxed);
            const offset_t loopStart = loopStart_.load(std::memory_order_relaxed);
            const offset_t loopEnd   = loopEnd_.load(std::memory_order_relaxed);
            const bool     loopValid = cycle && loopEnd > loopStart;

            // Allocate temporary buffers for L and R channels (pulled at graph rate).
            std::vector<float> bufL(frames), bufR(frames);
            std::size_t filled = 0;

            // Fill the buffers, wrapping back to loopStart whenever the cursor
            // reaches loopEnd. Without cycling this runs exactly one pull.
            while (filled < frames) {
                if (loopValid && pos >= loopEnd) {
                    if (SProject *proj = SApplication::app().getCurrentProject()) {
                        if (SObject *root = proj->getRootComponent())
                            root->seekTo(loopStart);
                    }
                    pos = loopStart;
                }

                length_t want = static_cast<length_t>(frames - filled);
                if (loopValid) {
                    double outLeft = (double)(loopEnd - pos) * rateRatio_;
                    length_t cap = (length_t) std::llround(outLeft);
                    if (cap < 1) cap = 1;
                    if (want > cap) want = cap;
                }

                length_t inConsumed = 0;
                length_t gotL = resamplerL_.process(inputL, bufL.data() + filled, want, &inConsumed);
                if (gotL <= 0) break;

                // Pull right channel with same consumed count.
                length_t gotR = 0;
                if (inputR != nullptr) {
                    gotR = resamplerR_.process(inputR, bufR.data() + filled, gotL, nullptr);
                    if (gotR < gotL) gotL = gotR;  // use the minimum
                } else {
                    // No right channel; copy left to right.
                    std::copy(bufL.begin() + filled, bufL.begin() + filled + gotL,
                              bufR.begin() + filled);
                }

                pos += (offset_t) inConsumed;
                filled += (std::size_t) gotL;

                if (!loopValid) break;
            }

            if (filled == 0) {
                std::fill_n(out, frames * channels, 0.0f);
                if (!SApplication::app().isRecordingActive())
                    SApplication::app().setGlobalLocatorPosRealtime(pos);
                return frames;
            }

            // Always output what we have (never pad with zeros - causes aliasing/artifacts)
            std::size_t outFrames = filled;

            // Interleave L/R into output. If channels > 2, duplicate the stereo
            // pair to all output channels.
            for (std::size_t i = 0; i < outFrames; ++i) {
                float sL = bufL[i];
                float sR = bufR[i];
                for (std::uint32_t c = 0; c < channels; ++c) {
                    out[i * channels + c] = (c % 2 == 0) ? sL : sR;
                }
            }

            // Pad any unfilled tail with silence (smooth, no aliasing).
            if (outFrames < frames) {
                for (std::size_t i = outFrames; i < frames; ++i) {
                    for (std::uint32_t c = 0; c < channels; ++c) {
                        out[i * channels + c] = 0.0f;
                    }
                }
            }

            if (!SApplication::app().isRecordingActive())
                SApplication::app().setGlobalLocatorPosRealtime(pos);
            return static_cast<std::size_t>(outFrames);
        });

    TWSPK_LOG( "calling backend->startOutput()" );
    if (backend_->startOutput() != 0) {
        TWSPK_LOG( "backend startOutput FAILED" );
        backend_->closeDevice();
        return;
    }
    TWSPK_LOG( "backend->startOutput() succeeded" );
    isPlaying_ = true;
}

void twSpeaker::stopOutput()
{
    std::lock_guard<std::mutex> lock(outputMutex_);
    if (!isPlaying_) { TWSPK_LOG( "already stopped" ); return; }
    TWSPK_LOG( "ENTER - stopping backend" );
    isPlaying_ = false;               // flip first: a re-entrant/observer sees "stopping"
    backend_->stopOutput();
    backend_->closeDevice();
    TWSPK_LOG( "stopped" );
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
    TWSPK_LOG( "output device set to '%s' (takes effect on next startOutput)",
               outputDeviceId_.c_str() );
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
    TWSPK_LOG( "entered." );
#endif
}
