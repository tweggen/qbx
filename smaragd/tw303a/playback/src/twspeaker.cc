#include "tw/playback/twspeaker.h"

#include "tw/devices/audio_backend.h"
#include "tw/playback/playback_context.h"
#include "tw/pages/io_vector.h"
#include "tw/graph/twnegotiator.h"
#include "tw/core/twsyslog.h"
#include "tw/core/twlog.h"
#include "tw/devices/midi_out_scheduler.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

// All diagnostic output in this file goes through the log sink under the
// "playback" category, tagged with the function so each line is self-locating
// and greppable. TwLog flushes the console tee immediately, so the last message
// before a crash/hang is still never lost in a buffer.
#define TWSPK_LOG( fmt, ... ) \
    TW_LOGD( "playback", "twSpeaker::%s: " fmt, __func__, ##__VA_ARGS__ )

twSpeaker::twSpeaker(tw303aEnvironment &env0)
    : twComponent(env0),
      backend_(audio::createAudioBackend()),
      isPlaying_(false)
{
    TWSPK_LOG( "using audio backend '%s'", backend_->name() );
}

twSpeaker::~twSpeaker()
{
    TWSPK_LOG( "destroying (isPlaying=%d, live=%d)", (int) isPlaying_.load(),
               (int) liveActive() );
    liveLane_.store(LiveLaneState::OFF, std::memory_order_release);
    if (deviceRunning_.load(std::memory_order_relaxed) || isPlaying_)
        backend_->stopOutput();
    backend_->closeDevice();
    deviceRunning_.store(false, std::memory_order_relaxed);
    deviceState_.store(DeviceState::CLOSED, std::memory_order_release);
}

std::shared_ptr<audio::AudioEngine> twSpeaker::engineSnapshot() const
{
    std::lock_guard<std::mutex> lock(engineMutex_);
    return std::atomic_load(&audioEngine_);
}

void twSpeaker::releaseEngine()
{
    std::shared_ptr<audio::AudioEngine> dying;
    {
        std::lock_guard<std::mutex> lock(engineMutex_);
        dying = std::atomic_load(&audioEngine_);
        std::atomic_store(&audioEngine_, std::shared_ptr<audio::AudioEngine>());
    }
    // The frozen lane is gone; what the clock last published describes a
    // playhead that no longer exists (design D2). The pump falls back to its
    // virtual counter on the next block.
    engineClock_.invalidate();
    // ~AudioEngine joins the readahead thread; run it with no lock held.
    dying.reset();
}

bool twSpeaker::startOutput()
{
    // Phase 1: Check and transition state atomically
    {
        std::lock_guard<std::mutex> lock(mutex());
        if (isPlaying_) return true;

        OutputState curState = outputState_.load(std::memory_order_relaxed);
        if (curState != OutputState::STOPPED) {
            TWSPK_LOG( "already starting or playing (state=%d)", (int)curState );
            return true;
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

    // THE ATTACH (proposal 21 L1a, design D5). When the live lane already has
    // the device open, Play must NOT re-open it: the callback is running, a
    // performer is hearing their input through it, and closing/reopening would
    // be an audible hole for no reason. ensureDeviceOpen() is a no-op then.
    const bool deviceWasOpen =
        ( deviceState_.load(std::memory_order_acquire) == DeviceState::OPEN );
    if (ensureDeviceOpen(graphRate) != 0) {
        std::lock_guard<std::mutex> lock(mutex());
        outputState_.store(OutputState::STOPPED, std::memory_order_relaxed);
        return false;
    }
    if (deviceWasOpen)
        TWSPK_LOG( "device already OPEN (live lane) — attaching the frozen lane "
                   "without re-opening" );

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
        return false;
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
    // THE PUBLISH-LAG CORRECTION, in PROJECT frames (design D2, and the same
    // conversion SMidiOutPump makes). cfg.bufferFrames is DEVICE frames at the
    // DEVICE rate; the position the callback publishes is a project frame.
    bufferFramesProject_.store(
        (std::uint64_t) ( (rateRatio_ > 0.0)
                              ? ( (double) cfg.bufferFrames / rateRatio_ )
                              : (double) cfg.bufferFrames ),
        std::memory_order_relaxed );

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
        // THE HANDLE SWAP UNDER A RUNNING CALLBACK (design D5). engineMutex_
        // stays a LEAF and is never taken by the callback; the store is atomic
        // so the callback's atomic_load either sees the old engine or the new
        // one, never a torn pointer. Before L1a this was a plain assignment,
        // which was safe only because the device could not be running yet.
        std::lock_guard<std::mutex> lock(engineMutex_);
        std::atomic_store(&audioEngine_, engine);
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

    // Phase 5: the callback is registered ONCE, by ensureDeviceOpen(), and
    // serves BOTH lanes for the life of the device session (design D5). It used
    // to be installed here, per start, which is exactly what made "attach
    // without re-opening" impossible.

    // DIAGNOSTIC: arm the effective-output-rate measurement. The
    // wall clock starts HERE rather than at the first callback, so the priming
    // wait is included and shows up as a rate BELOW the device rate; a device
    // whose clock genuinely runs slow shows the same shortfall proportional to
    // the whole run, so read the two apart by run length.
    outFramesDelivered_.store( 0, std::memory_order_relaxed );
    outputStartWall_ = std::chrono::steady_clock::now();

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
    return true;
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

    // DIAGNOSTIC — the OUTPUT twin of RecordingSession's
    // capture-rate check. A ratio well below 1 on a run of several seconds
    // means the device drained fewer frames than the rate we opened it at, so
    // the arrangement came out slow and flat; ~1.0 exonerates the device and
    // points the finger back at the graph.
    {
        const auto now = std::chrono::steady_clock::now();
        auto secsSince = [&now]( std::chrono::steady_clock::time_point t ) {
            return std::chrono::duration_cast<std::chrono::duration<double>>( now - t ).count();
        };
        const double wall = secsSince( outputStartWall_ );
        const std::uint64_t frames = outFramesDelivered_.load( std::memory_order_relaxed );
        const unsigned assumed = backend_ ? backend_->getConfig().sampleRate : 0u;
        const double eff = wall > 0.0 ? (double) frames / wall : 0.0;

        // The DEVICE window excludes the priming wait, so its ratio is the
        // device CLOCK alone. Split explicitly: ~1.0 here with a low overall
        // ratio means the audio was merely LATE by `priming`; a low ratio HERE
        // means the device really is draining slower than the rate we opened it
        // at, and 48 k content comes out flat.
        const bool started = outputDeviceStartWall_.time_since_epoch().count() != 0;
        const double devWall = started ? secsSince( outputDeviceStartWall_ ) : 0.0;
        const double devEff = devWall > 0.0 ? (double) frames / devWall : 0.0;
        const double priming = started
            ? std::chrono::duration_cast<std::chrono::duration<double>>(
                  outputDeviceStartWall_ - outputStartWall_ ).count()
            : wall;

        TWSPK_LOG( "output-rate check — assumed device=%u Hz, %llu frames. "
                   "SINCE START: %.1f Hz over %.3fs (ratio %.4f). "
                   "PRIMING: %.3fs. "
                   "SINCE DEVICE START: %.1f Hz over %.3fs (ratio %.4f)%s",
                   assumed, (unsigned long long) frames,
                   eff, wall, assumed > 0 ? eff / assumed : 0.0,
                   priming,
                   devEff, devWall, assumed > 0 ? devEff / assumed : 0.0,
                   started ? "" : "  [device never started]" );
    }

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

    // Phase 3/4: the device (no lock - potentially blocking I/O).
    //
    // WHILE THE LIVE LANE IS ON, THIS STOPS THE LANE AND NOT THE DEVICE (design
    // D5). The callback keeps running and keeps summing the ring; all that
    // changes is that its frozen half goes back to zero-filling. Stopping the
    // transport must not take a monitored input off the air.
    if (liveActive()) {
        TWSPK_LOG( "live lane is ON — stopping the FROZEN lane only; the device "
                   "stays open" );
    } else {
        if (curState == OutputState::PLAYING) {
            TWSPK_LOG( "stopping backend output" );
            backend_->stopOutput();  // Blocks until callback thread exits
        } else if (curState == OutputState::BUFFERING || curState == OutputState::OPENING) {
            TWSPK_LOG( "stopped before playback started (state=%d)", (int)curState );
        }
        closeDeviceNoLock();
    }

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

void twSpeaker::warmFrozenLane(offset_t pos)
{
    // MAIN THREAD ONLY, and never from the RT callback or the live pump: this
    // issues a DEMAND, which is exactly what those two may not do.
    if (!pageScheduler_ || !context_) return;
    std::shared_ptr<twComponent> root = context_->rootComponent();
    if (!root) return;

    const offset_t page  = (offset_t) twOutputPage::FRAME_CAPACITY;
    if (pos < 0) pos = 0;
    const offset_t start = twFloorAlign(pos, page);
    // Cover the SAME window startPlayback() will wait for, measured from the
    // playhead rather than from the page it sits in - one authority for the
    // number (AudioEngine::primingFrames), so a change there cannot leave the
    // warm-up covering less than the wait requires.
    const offset_t need  = (pos - start)
                         + (offset_t) audio::AudioEngine::primingFrames();
    int nPages = (int) ((need + page - 1) / page);
    if (nPages < 1) nPages = 1;

    // Priority 9, the readahead's own: this IS the readahead's work, done
    // early. A lower priority would let background aspect jobs go first and
    // spend the count-in on something nobody is waiting for.
    warmDemand_ = pageScheduler_->requestGraphPages(root, start, nPages, 9);
    TWSPK_LOG( "warmFrozenLane: demanded %d page(s) from %lld for a start at "
               "%lld (%.1f s of priming)",
               nPages, (long long) start, (long long) pos,
               (double) audio::AudioEngine::primingFrames() / 48000.0 );
}

bool twSpeaker::frozenLaneWarm() const
{
    return warmDemand_ && warmDemand_->done();
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
    // HOW LONG THE FROZEN LANE WAS HELD SILENT while the transport ran. Zero
    // is the goal and is reachable: with the pages already frozen (see
    // warmFrozenLane) and the readahead's first pass immediate, the very
    // first call below can answer PLAYING.
    primingPolls_.store(0, std::memory_order_relaxed);

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

                // THE PRIMING GATES THE FROZEN LANE, NEVER THE RING (design
                // D5). If the live lane already started the device, there is
                // nothing to start: the callback has been running and zero-
                // filling the frozen half, and all that changes here is that it
                // begins pulling.
                const bool alreadyRunning =
                    deviceRunning_.load(std::memory_order_acquire);
                if (alreadyRunning) {
                    TWSPK_LOG( "monitorReadaheadBuffer: device already running "
                               "(live lane) — frozen lane -> PLAYING" );
                    outputState_.store(OutputState::PLAYING, std::memory_order_relaxed);
                } else if (backend_->startOutput() == 0) {
                    TWSPK_LOG( "monitorReadaheadBuffer: backend->startOutput() succeeded" );
                    deviceRunning_.store(true, std::memory_order_release);
                    // DIAGNOSTIC: the instant the DEVICE actually
                    // began draining, so stopOutput can separate the one-off
                    // priming gap from a sustained clock error. Without this
                    // split the two are indistinguishable in one ratio.
                    outputDeviceStartWall_ = std::chrono::steady_clock::now();
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
                // and ~AudioEngine joins the readahead thread. The DEVICE is closed
                // only when the live lane does not own it.
                engine.reset();
                if (!liveActive()) closeDeviceNoLock();
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
        // 10 ms, not 50. This is the granularity of "how late the music is"
        // once the pages are warm, and it is pure sleeping - one atomic
        // compare per tick, bounded by the 10 s deadline above.
        primingPolls_.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
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
            if (!liveActive()) closeDeviceNoLock();
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

void twSpeaker::requestSeek(offset_t frame)
{
    if (frame < 0) frame = 0;
    // Snapshot the engine under engineMutex_ (same rationale as setCycle) and
    // post the seek; the engine adopts it on the RT thread. No engine == not
    // playing, so nothing to do (startOutput seeks from the locator itself).
    if (auto engine = engineSnapshot()) {
        engine->requestSeek((uint64_t) frame);
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

// ============================================================================
// THE LIVE LANE (proposal 21 L1a, design D5)
// ============================================================================

bool twSpeaker::probeOutputDevice()
{
    // Already proven: the live lane or a previous Play already has the device
    // open. No I/O, and — deliberately — no interaction with outputState_: a
    // probe must never look like a Play attempt to anything watching that
    // state machine.
    if (deviceState_.load(std::memory_order_acquire) == DeviceState::OPEN)
        return true;

    std::uint32_t graphRate = (std::uint32_t) env.getSRate();
    if (!pInputPlugs_.empty() && pInputPlugs_[0] != nullptr)
        graphRate = pInputPlugs_[0]->getFormat().sampleRate;

    TWSPK_LOG( "probeOutputDevice: calling openDevice with rate=%u, "
               "outputDeviceId=%s", graphRate, outputDeviceId_.c_str() );
    if (backend_->openDevice(outputDeviceId_, graphRate) != 0) {
        TWSPK_LOG( "probeOutputDevice: openDevice FAILED for '%s'",
                   outputDeviceId_.c_str() );
        return false;
    }
    backend_->closeDevice();
    TWSPK_LOG( "probeOutputDevice: openDevice/closeDevice succeeded for '%s'",
               outputDeviceId_.c_str() );
    return true;
}

int twSpeaker::ensureDeviceOpen(std::uint32_t rate)
{
    if (deviceState_.load(std::memory_order_acquire) == DeviceState::OPEN) return 0;

    TWSPK_LOG( "calling openDevice with rate=%u", rate );
    if (backend_->openDevice(outputDeviceId_, rate) != 0) {
        TWSPK_LOG( "openDevice FAILED" );
        return -1;
    }
    TWSPK_LOG( "openDevice succeeded" );

    const audio::AudioConfig cfg = backend_->getConfig();

    // The callback's planar scratch, sized ONCE per device session. It used to
    // be two std::vectors constructed per callback (recorded debt in the
    // CONTRACT); the live sum needs the planar buffers anyway, because it adds
    // into them BEFORE the interleave.
    const std::size_t maxFrames = std::max<std::size_t>( cfg.bufferFrames, 4096 );
    cbBuf_.assign( maxFrames * 2u, 0.0f );      // at most two pulled channels
    cbChans_.assign( 2u, nullptr );
    loggedCbGrow_.store( false, std::memory_order_relaxed );

    // The crossfade, at the DEVICE rate. 2.5 ms is the middle of design D2's
    // 2-3 ms band; setLiveCrossfadeMs() overrides it.
    if (liveCrossfadeFrames_.load(std::memory_order_relaxed) == 0)
        setLiveCrossfadeMs( 2.5 );

    backend_->setRenderCallback(
        [this](float *out, std::size_t frames, std::uint32_t channels) -> std::size_t {
            return renderCallbackBody( out, frames, channels );
        });

    deviceState_.store(DeviceState::OPEN, std::memory_order_release);
    return 0;
}

// Caller must hold NO lock: closeDevice() waits for the render thread.
void twSpeaker::closeDeviceNoLock()
{
    backend_->closeDevice();
    deviceRunning_.store(false, std::memory_order_release);
    deviceState_.store(DeviceState::CLOSED, std::memory_order_release);
}

void twSpeaker::setLiveCrossfadeMs(double ms)
{
    if (ms < 0.0) ms = 0.0;
    std::uint32_t rate = 48000;
    if (deviceState_.load(std::memory_order_acquire) == DeviceState::OPEN)
        rate = backend_->getConfig().sampleRate;
    liveCrossfadeFrames_.store( (std::uint32_t) ( ms * 0.001 * (double) rate ),
                                std::memory_order_relaxed );
}

int twSpeaker::openLive(std::uint32_t rate, idx_t channels)
{
    std::lock_guard<std::mutex> lock(mutex());
    if (liveActive()) return 0;

    if (channels < 1) channels = 1;

    const bool deviceWasOpen =
        (deviceState_.load(std::memory_order_acquire) == DeviceState::OPEN);

    if (ensureDeviceOpen(rate) != 0) return -1;

    const audio::AudioConfig cfg = backend_->getConfig();

    // THE RATE SCOPE (review fix 3). The live lane's entries are stamped in
    // PROJECT frames and summed straight into the device buffer; the frozen
    // lane has a resampler at this seam and the live lane has nowhere to put
    // one (a resampler makes a ring entry's frame count fractional, and an
    // entry has to carry a position). So the two rates must be equal, and when
    // they are not this REFUSES rather than monitoring off-pitch.
    if (cfg.sampleRate != rate) {
        liveRateRefusals_.fetch_add(1, std::memory_order_relaxed);
        TWSPK_LOG( "REFUSING the live lane: the device is at %u Hz and the project "
                   "at %u Hz. The live path carries PROJECT frames with no "
                   "resampler, so monitoring would be off-pitch. Open the device "
                   "at the project rate (playback/CONTRACT.md, known debt).",
                   (unsigned) cfg.sampleRate, (unsigned) rate );
        // Undo the open only if WE did it. A device the frozen lane is using
        // must survive a refused arm completely untouched.
        if (!deviceWasOpen &&
            outputState_.load(std::memory_order_relaxed) == OutputState::STOPPED) {
            if (deviceRunning_.load(std::memory_order_acquire)) backend_->stopOutput();
            closeDeviceNoLock();
        }
        return -1;
    }
    // The ring carries the PROJECT's channels, not the device's: the device
    // rule (playback inv. 9) is applied at the interleave, once, for both
    // lanes. Its entries are one device block each, 4 deep.
    liveRing_.reset( (std::uint32_t) channels,
                     (std::uint32_t) std::max<std::uint32_t>( cfg.bufferFrames, 1 ),
                     twLiveMixRing::kDefaultDepth );
    liveReader_.rewind();
    liveFade_ = twLiveMixState{};
    liveFade_.fadeFrames = liveCrossfadeFrames_.load(std::memory_order_relaxed);

    // NO PRIMING (design D5). There is no readahead behind a live lane and
    // nothing to buffer: the performer must hear the input the moment they arm.
    if (!deviceRunning_.load(std::memory_order_acquire)) {
        if (backend_->startOutput() != 0) {
            TWSPK_LOG( "openLive: backend->startOutput() FAILED" );
            closeDeviceNoLock();
            return -1;
        }
        deviceRunning_.store(true, std::memory_order_release);
    }

    liveLane_.store(LiveLaneState::ON, std::memory_order_release);
    TWSPK_LOG( "live lane ON (rate=%u, %d ch, %u-frame ring x %u)",
               (unsigned) cfg.sampleRate, (int) channels,
               (unsigned) liveRing_.framesPerEntry(), (unsigned) liveRing_.depth() );
    return 0;
}

void twSpeaker::closeLive()
{
    bool closeDevice = false;
    {
        std::lock_guard<std::mutex> lock(mutex());
        if (!liveActive()) return;
        liveLane_.store(LiveLaneState::OFF, std::memory_order_release);
        // The device goes only if the frozen lane is not using it. Read the
        // state, act outside the lock: closeDevice() waits for the render
        // thread and must never run under mutex().
        closeDevice = ( outputState_.load(std::memory_order_relaxed) == OutputState::STOPPED );
        TWSPK_LOG( "live lane OFF (device %s)",
                   closeDevice ? "closing" : "kept by the frozen lane" );
    }
    if (closeDevice) {
        if (deviceRunning_.load(std::memory_order_acquire)) backend_->stopOutput();
        closeDeviceNoLock();
    }
}

// ---------------------------------------------------------- the RT callback

std::size_t twSpeaker::renderCallbackBody(float *out, std::size_t frames,
                                          std::uint32_t channels)
{
    // Stage 6: mark the RT thread so twComponent::freezePage can ENFORCE
    // "the RT path never renders" (thread-local flag; a repeated store of
    // `true` is free).
    twRtThreadGuard::markRtThread();
    // DIAGNOSTIC (main, PR #56): count what the DEVICE actually drains, so
    // stopOutput can report an effective output rate. Every path below fills
    // `frames`, so it is counted here, once.
    outFramesDelivered_.fetch_add( (std::uint64_t) frames, std::memory_order_relaxed );

    // The engine handle, atomically: since proposal 21 L1a a NEW engine can be
    // minted while this callback is running (Play attaching to a device the
    // live lane opened), so a plain read of the shared_ptr would be a race.
    std::shared_ptr<audio::AudioEngine> engine = std::atomic_load(&audioEngine_);
    const bool liveOn = liveActive();

    // PROPOSAL 45 M3 / D4b: under a CLOSURE plan the pump renders the master
    // itself, so the frozen root page is not a second half to add -- it is the
    // SAME arrangement, unprocessed. Adding it would play everything twice.
    // Suppressed here rather than in the engine because this is the one place
    // that decides what reaches the device. (AC3.1's ENGINE half -- standing
    // the readahead and warmFrozenLane down from demanding root pages, so the
    // master lane's processors are not run on a worker while the pump renders
    // the same instances -- is a separate, unstarted piece of work.)
    const bool closureLive = liveOn && liveMasterClosure();

    // AND IT SUPPRESSES THE SUM, NOT THE PULL. `frozenPlaying` below gates
    // THREE things -- the pullBlock, the position publication + clock stamp at
    // the bottom of this function, and the live gate's position authority
    // (`gate.haveRoot` / `gate.wantPos = blockStart`) -- and only the AUDIO may
    // stand down. The first attempt folded `closureLive` into this flag and
    // suppressed all three; the transport position is published from
    // engine->currentPosition() AFTER the pull, so THE PLAYHEAD STOPPED.
    // Measured: the demand position stayed at 0 for a whole 5.2 s run,
    // SLiveMonitor::pumpDemands() re-demanded page 0 for ever, the pump's
    // frozen-input misses climbed to 62 against 4, and the unarmed track went
    // silent -- raising it 12 dB moved the captured output by 0.46 %.
    // D4b framed this as processor ownership; the CLOCK is a second thing the
    // frozen lane owns. See plan/proposed/45_SYSTEM_LANES.md, M3.
    const bool frozenPlaying =
        engine && engine->getPlaybackState() == audio::PlaybackState::PLAYING &&
        outputState_.load(std::memory_order_relaxed) == OutputState::PLAYING;

    if (!frozenPlaying && !liveOn) {
        // The pre-L1a behaviour, byte for byte: nothing to play, zero-fill.
        std::fill_n(out, frames * channels, 0.0f);
        return frames;
    }

    // THE DEVICE MONITORING RULE — stated in full at the top of twspeaker.h and
    // in playback/CONTRACT.md inv. 9:
    //     L = ch0;  R = (projectWidth >= 2) ? ch1 : ch0
    // and that pair meets the device's channel count as it always has. A
    // project wider than two is monitored on its FIRST TWO channels; the rest
    // are computed and dropped HERE, at the device, and never in a file.
    std::size_t projectWidth = 1;
    if (engine) projectWidth = std::max<std::size_t>(1, engine->graphChannels());
    else if (liveOn && liveRing_.channels() > 0)
        projectWidth = liveRing_.channels();
    const std::size_t pullCh = twmonitor::pullChannels(projectWidth);

    // ONE log line per width, not per callback.
    if ((int) projectWidth != monitorWidthLogged_.exchange((int) projectWidth,
                                                           std::memory_order_relaxed)) {
        if (projectWidth > 2)
            TWSPK_LOG("monitoring a %u-channel project on the device's FIRST TWO "
                      "channels (L=ch0, R=ch1); channels 2..%u are rendered and "
                      "dropped at the device. A render still gets all %u.",
                      (unsigned) projectWidth, (unsigned) (projectWidth - 1),
                      (unsigned) projectWidth);
        else
            TWSPK_LOG("monitoring a %u-channel project (L=ch0, R=%s)",
                      (unsigned) projectWidth,
                      projectWidth >= 2 ? "ch1" : "ch0");
    }

    if (cbBuf_.size() < pullCh * frames) {
        // A device block larger than the one the session opened with. Growing
        // here allocates on the RT path exactly once per surprise, which is
        // strictly better than the per-callback allocation this replaced.
        cbBuf_.assign(pullCh * frames, 0.0f);
        if (!loggedCbGrow_.exchange(true, std::memory_order_relaxed))
            TWSPK_LOG("callback block grew to %u frames x %u ch; scratch resized once",
                      (unsigned) frames, (unsigned) pullCh);
    }
    if (cbChans_.size() < pullCh) cbChans_.assign(pullCh, nullptr);
    for (std::size_t c = 0; c < pullCh; ++c) cbChans_[c] = cbBuf_.data() + c * frames;
    float *const *chans = cbChans_.data();

    // --- the FROZEN lane ----------------------------------------------------
    std::size_t   outFrames = 0;
    bool          haveRoot  = false;
    std::uint64_t rootEpoch = 0;
    std::int64_t  blockStart = -1;

    if (frozenPlaying) {
        blockStart = (std::int64_t) engine->currentPosition();
        outFrames  = (std::size_t) engine->pullBlock(chans, pullCh, frames);
        if (outFrames > 0) {
            haveRoot  = true;
            rootEpoch = engine->servedContentEpoch();
        }
        // pullBlock() writes every buffer for the full nFrames on every path,
        // including the misses — so the planar scratch is defined either way.

        // THE CLOSURE SUPPRESSION, and this is the whole of it: drop the
        // AUDIO the pull produced and keep everything else it produced --
        // `blockStart`, `haveRoot`, `rootEpoch`, and (below) the position
        // publication and the clock stamp. The engine has advanced, so the
        // demands follow the playhead and the pages stay warm; the device
        // simply does not hear this copy of the arrangement.
        if (closureLive) {
            std::fill(cbBuf_.begin(),
                      cbBuf_.begin() + (std::ptrdiff_t)(pullCh * frames), 0.0f);
        }
    } else {
        std::fill(cbBuf_.begin(), cbBuf_.begin() + (std::ptrdiff_t)(pullCh * frames), 0.0f);
    }

    // --- the LIVE lane ------------------------------------------------------
    //
    // THE RING IS A STREAM, NOT A BLOCK QUEUE (review fix 1). This callback's
    // `frames` is whatever the device asked for -- VARIABLE on WASAPI, which
    // hands us `bufferFrames - padding` -- while the pump produces fixed
    // blocks, so the two grids never line up and an equality test would leave
    // the live lane permanently silent on real hardware. mixStream() consumes
    // by FRAME RANGE, with a cursor that lives across callbacks.
    if (liveOn) {
        twLiveRingEntry head;
        const bool haveHead = liveRing_.peek(head);

        twLiveMixGate gate;
        // WHILE STOPPED THERE IS NO ROOT PAGE: out = ring (design D2), and the
        // stream is consumed sequentially from wherever its head sits -- the
        // ring is then the only position authority there is. With the frozen
        // lane playing, the engine's position is the authority.
        gate.wantPos   = haveRoot
                             ? blockStart
                             : (haveHead ? head.startPos + (std::int64_t) liveReader_.cursor
                                         : 0);
        gate.rootEpoch = rootEpoch;
        gate.haveRoot  = haveRoot;

        const bool wantFadeOut = (haveHead && head.flipEpochPrime != 0);
        if (wantFadeOut != liveFade_.fadingOut) {
            liveFade_.fadingOut = wantFadeOut;
            liveFade_.fadeDone  = 0;
        }
        liveFade_.fadeFrames = liveCrossfadeFrames_.load(std::memory_order_relaxed);

        if (frozenPlaying && outFrames < frames) {
            // The ring covers the whole block even where the frozen lane ran
            // short, so the tail must be defined before the sum.
            for (std::size_t c = 0; c < pullCh; ++c)
                std::fill(chans[c] + outFrames, chans[c] + frames, 0.0f);
            outFrames = frames;
        }

        twLiveStreamStats st;
        twlive::mixStream(chans, pullCh, frames, gate.wantPos, gate,
                          liveRing_, liveReader_, liveFade_, st);
        if (st.framesSummed > 0 && !haveRoot) outFrames = frames;
    }

    if (outFrames == 0) {
        std::fill_n(out, frames * channels, 0.0f);
        if (frozenPlaying && context_)
            context_->publishPosition(engine->currentPosition());
        return frames;
    }

    twmonitor::interleave(out, outFrames, channels,
                          const_cast<const float *const *>(chans), pullCh);

    if (outFrames < frames) {
        std::fill_n(out + outFrames * channels,
                    (frames - outFrames) * channels, 0.0f);
    }

    if (frozenPlaying) {
        const std::uint64_t pos = engine->currentPosition();
        // THE PLAYHEAD AUTHORITY, in every mode (proposal 21 L3b, design D7).
        // `locatorHeldElsewhere()` is retired: a recording is an ordinary
        // transport run now, and the record worker no longer drives the
        // locator, so there is nobody else to hold it.
        if (context_)
            context_->publishPosition(pos);
        // THE LIVE CLOCK, stamped beside the publication and derived from it by
        // the one publish-lag correction (design D2 / twliveclock.h).
        const std::uint64_t lag = bufferFramesProject_.load(std::memory_order_relaxed);
        // deliveredFrame is what is being HEARD (publish lag removed), which is
        // what MIDI-out and metering want; nextFrame is what the RT will PULL
        // next, which is the published position itself, and is what the PUMP
        // paces on (review fix 2). One stamp, two readings, no conflation.
        engineClock_.stamp( (std::int64_t) pos - (std::int64_t) lag,
                            (std::int64_t) pos,
                            audio::MidiOutScheduler::hostNowNs() );
    }
    return outFrames;
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
