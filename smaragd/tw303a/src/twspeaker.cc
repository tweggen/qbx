#include "twspeaker.h"

#include "audio/audio_backend.h"
#include "io_vector.h"
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

    OutputState curState = outputState_.load(std::memory_order_relaxed);
    if (curState != OutputState::STOPPED) {
        TWSPK_LOG( "already starting or playing (state=%d)", (int)curState );
        return;
    }

    TWSPK_LOG( "ENTER - backend=%p, outputDeviceId=%s",
               backend_.get(), outputDeviceId_.c_str() );

    // Transition to OPENING state
    outputState_.store(OutputState::OPENING, std::memory_order_relaxed);

    // The graph (synth) runs at its input wire's rate — the project/env rate.
    // Request that rate from the device so that, when the device can open there,
    // no resampling is needed at all.
    std::uint32_t graphRate = (std::uint32_t) env.getSRate();
    if (!pInputPlugs_.empty() && pInputPlugs_[0] != nullptr) {
        graphRate = pInputPlugs_[0]->getFormat().sampleRate;
    }

    TWSPK_LOG( "calling openDevice with rate=%u", graphRate );
    if (backend_->openDevice(outputDeviceId_, graphRate) != 0) {
        TWSPK_LOG( "openDevice FAILED" );
        outputState_.store(OutputState::STOPPED, std::memory_order_relaxed);
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
        outputState_.store(OutputState::STOPPED, std::memory_order_relaxed);
        return;
    }

    // Destroy old audioEngine_ before creating new one to avoid use-after-free
    audioEngine_.reset();

    audioEngine_ = std::make_unique<audio::AudioEngine>(synthOutput, graphRate);
    audioEngine_->configureResampling(graphRate, cfg.sampleRate);
    audioEngine_->startReadahead();  // Start read-ahead thread to pre-compute pages

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
        std::uint32_t wireRate = (!pInputPlugs_.empty() && pInputPlugs_[0] != nullptr)
                                     ? pInputPlugs_[0]->getFormat().sampleRate
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

    // Transition to BUFFERING state
    outputState_.store(OutputState::BUFFERING, std::memory_order_relaxed);
    isPlaying_ = true;  // Set flag for UI status checks

    // Spawn background task to monitor readahead buffer and start backend output when ready
    // Phase 6b: Defer backend_->startOutput() until readahead has buffered 3+ seconds
    bufferingTaskRunning_.store(true, std::memory_order_relaxed);
    if (bufferingTask_.joinable()) bufferingTask_.join();  // Clean up old thread if any
    bufferingTask_ = std::thread([this] { monitorReadaheadBuffer(); });

    TWSPK_LOG( "transitioned to BUFFERING; background task monitoring readahead" );
}

void twSpeaker::stopOutput()
{
    std::lock_guard<std::mutex> lock(outputMutex_);

    OutputState curState = outputState_.load(std::memory_order_relaxed);
    if (curState == OutputState::STOPPED) {
        TWSPK_LOG( "already stopped" );
        return;
    }

    TWSPK_LOG( "ENTER - stopping (state=%d)", (int)curState );
    isPlaying_ = false;  // flip first: observers see "stopping"

    // Transition to STOPPING state
    outputState_.store(OutputState::STOPPING, std::memory_order_relaxed);

    // Cancel buffering task if still running
    bufferingTaskRunning_.store(false, std::memory_order_relaxed);
    if (bufferingTask_.joinable()) {
        bufferingTask_.join();
    }

    // If in PLAYING state, stop the backend callback
    if (curState == OutputState::PLAYING) {
        TWSPK_LOG( "stopping backend output" );
        backend_->stopOutput();
    } else if (curState == OutputState::BUFFERING || curState == OutputState::OPENING) {
        TWSPK_LOG( "stopped before playback started (state=%d)", (int)curState );
    }

    backend_->closeDevice();
    audioEngine_.reset();  // Ensure audioEngine destroyed before returning

    // Transition to STOPPED state
    outputState_.store(OutputState::STOPPED, std::memory_order_relaxed);
    TWSPK_LOG( "stopped" );
}

bool twSpeaker::isPlaying()
{
    return isPlaying_;
}

void twSpeaker::monitorReadaheadBuffer()
{
    // Phase 6b: Background task that waits for readahead buffer to be ready,
    // then starts the backend output callback.
    // This runs in a background thread, spawned from startOutput().

    if (!audioEngine_) {
        TWSPK_LOG( "monitorReadaheadBuffer: audioEngine is null, exiting" );
        return;
    }

    TWSPK_LOG( "monitorReadaheadBuffer: waiting for playback buffer to be ready (3+ sec)..." );

    // Wait on the readahead buffer ready condition variable with a 10-second timeout
    std::unique_lock<std::mutex> lk(outputMutex_);
    auto &cv = audioEngine_->getPlaybackReadyCv();

    // Use a dummy mutex for the CV (CV requires a mutex to hold during wait)
    // Actually, we can use a temporary local approach. The CV is owned by audioEngine_.
    // We need to wait without holding outputMutex_ to avoid deadlock.
    lk.unlock();

    // Create a local mutex for waiting on the CV
    std::mutex cvMutex;
    std::unique_lock<std::mutex> cvLock(cvMutex);
    bool timed_out = false;

    // Wait for the readahead buffer to reach minimum size (signaled via playbackReadyCv_)
    // Use a simple polling approach with timeout instead of direct CV wait
    // (since we don't have exclusive ownership of the CV's mutex)
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);

    while (bufferingTaskRunning_.load(std::memory_order_relaxed)) {
        // Check if audioEngine has reached PLAYING state
        if (audioEngine_ && audioEngine_->getPlaybackState() == audio::PlaybackState::PLAYING) {
            TWSPK_LOG( "monitorReadaheadBuffer: buffer ready, starting backend output" );
            // Lock outputMutex_ before accessing backend and state
            std::lock_guard<std::mutex> lock(outputMutex_);

            // Verify we're still in BUFFERING state (haven't been stopped)
            if (outputState_.load(std::memory_order_relaxed) == OutputState::BUFFERING) {
                // Now safe to call backend_->startOutput()
                if (backend_->startOutput() == 0) {
                    TWSPK_LOG( "monitorReadaheadBuffer: backend->startOutput() succeeded" );
                    outputState_.store(OutputState::PLAYING, std::memory_order_relaxed);
                } else {
                    TWSPK_LOG( "monitorReadaheadBuffer: backend->startOutput() FAILED" );
                    outputState_.store(OutputState::STOPPED, std::memory_order_relaxed);
                    isPlaying_ = false;
                    return;
                }
            }
            return;
        }

        // Check timeout
        if (std::chrono::steady_clock::now() >= deadline) {
            TWSPK_LOG( "monitorReadaheadBuffer: TIMEOUT waiting for buffer (>10 sec), stopping playback" );
            fprintf(stderr, "[twSpeaker] Readahead timeout (>10 sec); stopping playback\n");
            fflush(stderr);
            timed_out = true;
            break;
        }

        // Sleep for 50ms before checking again
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (timed_out || !bufferingTaskRunning_.load(std::memory_order_relaxed)) {
        // Timeout or stop signal; clean up
        std::lock_guard<std::mutex> lock(outputMutex_);
        if (outputState_.load(std::memory_order_relaxed) == OutputState::BUFFERING) {
            TWSPK_LOG( "monitorReadaheadBuffer: cleaning up after timeout/stop" );
            outputState_.store(OutputState::STOPPED, std::memory_order_relaxed);
            isPlaying_ = false;
            backend_->closeDevice();
            audioEngine_.reset();
        }
    }
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

// Phase 3: IOVector-based interface (type-safe, page-backed rendering)
// twSpeaker is an output sink, not a source — calcOutputTo is a stub
length_t twSpeaker::calcOutputTo(IOVector& dest, idx_t)
{
    // Output sink: fill destination with silence
    return dest.fillSilence(0, dest.length());
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
