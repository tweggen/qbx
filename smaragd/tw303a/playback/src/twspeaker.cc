#include "tw/playback/twspeaker.h"

#include "tw/devices/audio_backend.h"
#include "tw/playback/playback_context.h"
#include "tw/pages/io_vector.h"
#include "tw/graph/twnegotiator.h"
#include "tw/core/twsyslog.h"

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

std::shared_ptr<audio::AudioEngine> twSpeaker::engineSnapshot() const
{
    std::lock_guard<std::mutex> lock(engineMutex_);
    return audioEngine_;
}

void twSpeaker::releaseEngine()
{
    std::shared_ptr<audio::AudioEngine> dying;
    {
        std::lock_guard<std::mutex> lock(engineMutex_);
        dying.swap(audioEngine_);
    }
    // ~AudioEngine joins the readahead thread; run it with no lock held.
    dying.reset();
}

void twSpeaker::startOutput()
{
    // Phase 1: Check and transition state atomically
    {
        std::lock_guard<std::mutex> lock(mutex());
        if (isPlaying_) return;

        OutputState curState = outputState_.load(std::memory_order_relaxed);
        if (curState != OutputState::STOPPED) {
            TWSPK_LOG( "already starting or playing (state=%d)", (int)curState );
            return;
        }

        TWSPK_LOG( "ENTER - backend=%p, outputDeviceId=%s",
                   backend_.get(), outputDeviceId_.c_str() );

        outputState_.store(OutputState::OPENING, std::memory_order_relaxed);
    } // Release stateMutex_ before expensive I/O

    // Phase 2: Expensive I/O (no lock held)
    std::uint32_t graphRate = (std::uint32_t) env.getSRate();
    if (!pInputPlugs_.empty() && pInputPlugs_[0] != nullptr) {
        graphRate = pInputPlugs_[0]->getFormat().sampleRate;
    }

    TWSPK_LOG( "calling openDevice with rate=%u", graphRate );
    if (backend_->openDevice(outputDeviceId_, graphRate) != 0) {
        TWSPK_LOG( "openDevice FAILED" );
        {
            std::lock_guard<std::mutex> lock(mutex());
            outputState_.store(OutputState::STOPPED, std::memory_order_relaxed);
        }
        return;
    }
    TWSPK_LOG( "openDevice succeeded" );

    // Negotiate rates (no lock needed)
    {
        twNegotiator negotiator(env);
        negotiator.negotiate(shared_from_this(), backend_->supportedRates());
    }

    const audio::AudioConfig cfg = backend_->getConfig();

    // Get synth output component from the app context (no lock needed)
    std::shared_ptr<twComponent> synthOutput = context_->rootComponent();

    if (!synthOutput) {
        TWSPK_LOG( "ERROR: Could not get synth output component" );
        backend_->closeDevice();
        {
            std::lock_guard<std::mutex> lock(mutex());
            outputState_.store(OutputState::STOPPED, std::memory_order_relaxed);
        }
        return;
    }

    // Phase 3: Create engine. Destroy the old one first, outside engineMutex_ —
    // ~AudioEngine joins the readahead thread and must not run under a lock.
    releaseEngine();

    auto engine = std::make_shared<audio::AudioEngine>(synthOutput, graphRate);

    // Phase 4: Configure the engine before publishing the handle. Nothing else can
    // observe it yet (outputState_ is OPENING, so no concurrent start/stop gets past
    // Phase 1), so this needs no lock.
    engine->configureResampling(graphRate, cfg.sampleRate);
    // Stage 5: hand the project's page scheduler to the engine — the readahead
    // becomes a demand consumer (set BEFORE startReadahead below).
    engine->setScheduler(pageScheduler_);

    rateRatio_ = (graphRate > 0) ? ((double) cfg.sampleRate / (double) graphRate) : 1.0;

    engine->setLoopBoundaries(
        cycleEnabled_.load(std::memory_order_relaxed),
        loopStart_.load(std::memory_order_relaxed),
        loopEnd_.load(std::memory_order_relaxed)
    );

    // Seek to the current playhead BEFORE starting the readahead: seekTo resets
    // the readahead frontier/state-chain, so starting the thread first would let
    // it freeze pages from position 0 and race the reset (unsynchronized frontier
    // writes from two threads).
    engine->seekTo((offset_t) context_->locatorPosition());
    engine->startReadahead();

    // Publish the handle (leaf-lock write).

    {
        std::lock_guard<std::mutex> lock(engineMutex_);
        audioEngine_ = engine;
    }

    // Rate diagnostics
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

    // Phase 5: Register callback (no lock needed)
    backend_->setRenderCallback(
        [this](float *out, std::size_t frames, std::uint32_t channels) -> std::size_t {
            auto engine = audioEngine_;  // Lock-free capture via shared_ptr
            if (!engine) {
                std::fill_n(out, frames * channels, 0.0f);
                return frames;
            }

            // Defensive: Only pull audio if engine is in PLAYING state (Phase 6b safety)
            if (engine->getPlaybackState() != audio::PlaybackState::PLAYING) {
                std::fill_n(out, frames * channels, 0.0f);
                return frames;
            }

            std::vector<float> bufL(frames), bufR(frames);
            std::size_t outFrames = engine->pullBlock(bufL.data(), bufR.data(), frames);

            if (outFrames == 0) {
                std::fill_n(out, frames * channels, 0.0f);
                if (context_ && !context_->locatorHeldElsewhere())
                    context_->publishPosition(engine->currentPosition());
                return frames;
            }

            for (std::size_t i = 0; i < outFrames; ++i) {
                float sL = bufL[i];
                float sR = bufR[i];
                for (std::uint32_t c = 0; c < channels; ++c) {
                    out[i * channels + c] = (c % 2 == 0) ? sL : sR;
                }
            }

            if (outFrames < frames) {
                for (std::size_t i = outFrames; i < frames; ++i) {
                    for (std::uint32_t c = 0; c < channels; ++c) {
                        out[i * channels + c] = 0.0f;
                    }
                }
            }

            if (context_ && !context_->locatorHeldElsewhere())
                context_->publishPosition(engine->currentPosition());
            return static_cast<std::size_t>(outFrames);
        });

    // Phase 6: Transition to BUFFERING and spawn monitor task
    {
        std::lock_guard<std::mutex> lock(mutex());
        outputState_.store(OutputState::BUFFERING, std::memory_order_relaxed);
        isPlaying_ = true;
    }

    // Spawn background task (brief taskMutex_ hold)
    {
        std::lock_guard<std::mutex> lock(taskMutex_);
        bufferingTaskRunning_.store(true, std::memory_order_relaxed);
        if (bufferingTask_.joinable()) bufferingTask_.join();
        bufferingTask_ = std::thread([this] { monitorReadaheadBuffer(); });
    }

    TWSPK_LOG( "transitioned to BUFFERING; background task monitoring readahead" );
}

void twSpeaker::stopOutput()
{
    // Phase 1: Check state and mark as stopping (brief lock)
    OutputState curState;
    {
        std::lock_guard<std::mutex> lock(mutex());
        curState = outputState_.load(std::memory_order_relaxed);
        if (curState == OutputState::STOPPED) {
            TWSPK_LOG( "already stopped" );
            return;
        }

        TWSPK_LOG( "ENTER - stopping (state=%d)", (int)curState );
        isPlaying_ = false;
        outputState_.store(OutputState::STOPPING, std::memory_order_relaxed);
    } // Release stateMutex_

    // Phase 2: Stop buffering task (brief lock, may join)
    {
        std::lock_guard<std::mutex> lock(taskMutex_);
        bufferingTaskRunning_.store(false, std::memory_order_relaxed);
        if (bufferingTask_.joinable()) {
            // Releasing taskMutex_ before the join would be ideal, but std::thread
            // doesn't support detach+rejoin safely. Keep this brief since monitorReadaheadBuffer()
            // doesn't hold taskMutex_ except during its own thread creation.
            bufferingTask_.join();
        }
    } // Release taskMutex_

    // Phase 3: Stop backend output (no lock - potentially blocking I/O)
    if (curState == OutputState::PLAYING) {
        TWSPK_LOG( "stopping backend output" );
        backend_->stopOutput();  // Blocks until callback thread exits
    } else if (curState == OutputState::BUFFERING || curState == OutputState::OPENING) {
        TWSPK_LOG( "stopped before playback started (state=%d)", (int)curState );
    }

    // Phase 4: Close device (no lock - potentially blocking I/O)
    backend_->closeDevice();

    // Phase 5: Destroy engine (handle cleared under engineMutex_, destructor runs unlocked)
    releaseEngine();

    // Phase 6: Final state transition (brief lock)
    {
        std::lock_guard<std::mutex> lock(mutex());
        outputState_.store(OutputState::STOPPED, std::memory_order_relaxed);
    }

    TWSPK_LOG( "stopped" );
}

bool twSpeaker::isPlaying()
{
    return isPlaying_;
}

void twSpeaker::monitorReadaheadBuffer()
{
    // Phase 6b: Background task that polls readahead progress and starts backend output
    // when buffer is ready. Runs in background thread spawned from startOutput().
    //
    // Lock discipline (see twspeaker.h): engineMutex_ is a leaf lock, only ever held to
    // read the audioEngine_ handle. Everything below works on a local shared_ptr copy, so
    // mutex() is never acquired with engineMutex_ held, and neither lock is held across
    // backend I/O or engine destruction.

    if (!engineSnapshot()) {
        TWSPK_LOG( "monitorReadaheadBuffer: audioEngine is null, exiting" );
        return;
    }

    TWSPK_LOG( "monitorReadaheadBuffer: waiting for playback buffer to be ready (3+ sec)..." );

    // Poll readahead progress with 10-second timeout
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    bool timed_out = false;

    while (bufferingTaskRunning_.load(std::memory_order_relaxed)) {
        std::shared_ptr<audio::AudioEngine> engine = engineSnapshot();
        if (!engine) {
            TWSPK_LOG( "monitorReadaheadBuffer: audioEngine went away, exiting" );
            return;
        }

        // Critical: Call startPlayback() to update state, don't just read cached value
        if (engine->startPlayback() == audio::PlaybackState::PLAYING) {
            TWSPK_LOG( "monitorReadaheadBuffer: buffer ready, starting backend output" );

            bool startFailed = false;
            {
                std::lock_guard<std::mutex> stateLock(mutex());
                if (outputState_.load(std::memory_order_relaxed) != OutputState::BUFFERING) {
                    // stopOutput() has taken over; it owns the teardown.
                    return;
                }

                if (backend_->startOutput() == 0) {
                    TWSPK_LOG( "monitorReadaheadBuffer: backend->startOutput() succeeded" );
                    outputState_.store(OutputState::PLAYING, std::memory_order_relaxed);
                } else {
                    TWSPK_LOG( "monitorReadaheadBuffer: backend->startOutput() FAILED" );
                    // Hold BUFFERING until the device is released below: a startOutput()
                    // that saw STOPPED could otherwise openDevice() while we close it.
                    outputState_.store(OutputState::STOPPING, std::memory_order_relaxed);
                    isPlaying_ = false;
                    startFailed = true;
                }
            }

            if (startFailed) {
                // Teardown with no lock held: closeDevice() waits for the render thread
                // and ~AudioEngine joins the readahead thread.
                engine.reset();
                backend_->closeDevice();
                releaseEngine();
                std::lock_guard<std::mutex> stateLock(mutex());
                outputState_.store(OutputState::STOPPED, std::memory_order_relaxed);
            }
            return;
        }

        // Check timeout
        if (std::chrono::steady_clock::now() >= deadline) {
            TWSPK_LOG( "monitorReadaheadBuffer: TIMEOUT waiting for buffer (>10 sec), stopping playback" );
            timed_out = true;
            break;
        }

        // Sleep for 50ms before checking again
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (timed_out || !bufferingTaskRunning_.load(std::memory_order_relaxed)) {
        // Timeout or stop signal. Claim the teardown by moving BUFFERING -> STOPPING
        // under mutex(); if we don't win the race, stopOutput() is already doing it.
        bool ownsTeardown = false;
        {
            std::lock_guard<std::mutex> stateLock(mutex());
            if (outputState_.load(std::memory_order_relaxed) == OutputState::BUFFERING) {
                TWSPK_LOG( "monitorReadaheadBuffer: cleaning up after timeout/stop" );
                outputState_.store(OutputState::STOPPING, std::memory_order_relaxed);
                isPlaying_ = false;
                ownsTeardown = true;
            }
        }

        if (ownsTeardown) {
            backend_->closeDevice();
            releaseEngine();
            std::lock_guard<std::mutex> stateLock(mutex());
            outputState_.store(OutputState::STOPPED, std::memory_order_relaxed);
        }
    }
}

void twSpeaker::setCycle(bool enabled, offset_t startFrame, offset_t endFrame)
{
    // An empty or inverted range can't loop; treat it as cycle-off.
    if (endFrame <= startFrame) enabled = false;

    // Store loop parameters in atomics (lock-free on audio thread)
    loopStart_.store(startFrame, std::memory_order_relaxed);
    loopEnd_.store(endFrame, std::memory_order_relaxed);
    cycleEnabled_.store(enabled, std::memory_order_relaxed);

    // Snapshot the engine under engineMutex_ and call it with the lock released. The
    // shared_ptr copy keeps the engine alive even if stopOutput() clears the handle
    // concurrently, so no lock needs to be held across the call.
    if (auto engine = engineSnapshot()) {
        engine->setLoopBoundaries(enabled, startFrame, endFrame);
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
