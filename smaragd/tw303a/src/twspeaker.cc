#include "twspeaker.h"

#include "audio/audio_backend.h"
#include "sapplication.h"
#include "sproject.h"
#include "sobject.h"
#include "twnegotiator.h"
#include "twsyslog.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

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

    // Create or reconfigure the audio engine for this session
    // Get the synth output component from the current project
    twComponent *synthOutput = nullptr;
    if (SProject *proj = SApplication::app().getCurrentProject()) {
        if (SObject *root = proj->getRootComponent()) {
            synthOutput = &root->getRootComponent();
        }
    }

    if (!synthOutput) {
        TWSPK_LOG( "ERROR: Could not get synth output component" );
        backend_->closeDevice();
        return;
    }

    // Destroy old audioEngine_ before creating new one to avoid use-after-free
    audioEngine_.reset();

    audioEngine_ = std::make_unique<audio::AudioEngine>(synthOutput, graphRate);
    audioEngine_->configureResampling(graphRate, cfg.sampleRate);

    // Output-to-input frame ratio, used to bound a pull at the loop end during
    // cycle playback. 1.0 when the resampler is a passthrough.
    rateRatio_ = (graphRate > 0) ? ((double) cfg.sampleRate / (double) graphRate) : 1.0;

    // Sync loop boundaries into the engine
    audioEngine_->setLoopBoundaries(
        cycleEnabled_.load(std::memory_order_relaxed),
        loopStart_.load(std::memory_order_relaxed),
        loopEnd_.load(std::memory_order_relaxed)
    );

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
        bool isPassthrough = (graphRate == cfg.sampleRate);
        TWSPK_LOG( "rate diag — project(env)=%d Hz, wire=%u Hz, "
                   "device=%u Hz, resampler=%s",
                   env.getSRate(), (unsigned) wireRate, (unsigned) cfg.sampleRate,
                   isPassthrough ? "passthrough" : "active" );
    }

    if (graphRate != cfg.sampleRate) {
        TWSPK_LOG( "resampling %u Hz -> %u Hz",
                   (unsigned) graphRate, (unsigned) cfg.sampleRate );
    }

    backend_->setRenderCallback(
        [this](float *out, std::size_t frames, std::uint32_t channels) -> std::size_t {
            // CRITICAL: Capture shared_ptr to audioEngine in callback scope to prevent
            // use-after-free. Even if stopOutput() sets this->audioEngine_ = nullptr,
            // this local copy keeps the engine alive during callback execution.
            // This is safe because stopOutput() blocks until the callback thread exits,
            // so the local engine reference is destroyed before stopOutput() returns.
            auto engine = audioEngine_;
            if (!engine) {
                std::fill_n(out, frames * channels, 0.0f);
                return frames;
            }

            // Pull stereo frames via AudioEngine (unified component graph pull)
            std::vector<float> bufL(frames), bufR(frames);

            // Pull block from AudioEngine (handles resampling, loop cycling, position tracking)
            std::size_t outFrames = engine->pullBlock(bufL.data(), bufR.data(), frames);

            if (outFrames == 0) {
                std::fill_n(out, frames * channels, 0.0f);
                if (!SApplication::app().isRecordingActive())
                    SApplication::app().setGlobalLocatorPosRealtime(engine->currentPosition());
                return frames;
            }

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
                SApplication::app().setGlobalLocatorPosRealtime(engine->currentPosition());
            return static_cast<std::size_t>(outFrames);
        });

    // Seek engine to current playhead position
    if (audioEngine_) {
        offset_t pos = SApplication::app().getGlobalLocatorPos();
        audioEngine_->seekTo(pos);
    }

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
    audioEngine_.reset();              // Ensure audioEngine destroyed before returning
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

    // CRITICAL: Acquire lock to prevent race with stopOutput() destroying audioEngine_.
    // stopOutput() holds this lock while calling backend_->stopOutput() (which blocks
    // the audio thread) and then destroys audioEngine_. We must hold the same lock
    // to ensure audioEngine_ is not being destroyed while we access it.
    // The lock scope is brief: only for the setLoopBoundaries call.
    std::lock_guard<std::mutex> lock(outputMutex_);

    // Now safe to access audioEngine_ while holding the lock that protects startOutput/stopOutput
    if (audioEngine_) {
        audioEngine_->setLoopBoundaries(enabled, startFrame, endFrame);
    }
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


void twSpeaker::reset()
{
	// Speaker sink: output device, no component state to reset
	// AudioEngine resampling state is managed separately
}
