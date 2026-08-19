#ifndef _TWSPEAKER_H_
#define _TWSPEAKER_H_

#include "tw/graph/twcomponent.h"
#include "tw/sources/twresampler.h"
#include "tw/devices/audio_backend.h"
#include "tw/playback/audio_engine.h"
#include "tw/playback/playback_context.h"
#include "tw/playback/twliveclock.h"
#include "tw/playback/twlivering.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ============================================================================
// THE DEVICE MONITORING RULE (proposal 36 B5) — requester decision, 2026-08-16.
//
// twSpeaker already owns the SAMPLE-RATE mismatch at the device boundary, so it
// owns the CHANNEL mismatch too. The rule is deliberately one line rather than a
// general N-to-M mapping, because monitoring is a stereo activity and a
// fold-down needs channel ROLES and a fold law that proposal 36 §8 names as an
// explicit non-goal:
//
//     L = ch0;   R = (projectWidth >= 2) ? ch1 : ch0;
//
// so a MONO project is standard mono-to-stereo, a stereo project is itself, and
// a project WIDER than two is monitored on its FIRST TWO CHANNELS — the rest
// are computed in full and DROPPED AT THE DEVICE, deliberately. That pair then
// meets the device's own channel count exactly as it always has, alternating
// across however many outputs the device has.
//
// THIS IS THE DEVICE PATH ONLY AND IT MUST NOT TOUCH THE FILE. A render is
// RenderSession, in tw/render, which shares no code with this: it writes the
// project's FULL width (AC B5.3 — a 6-channel project renders a 6-channel
// file). A 6-channel project that is monitored in stereo still renders six
// channels.
//
// It is a pure free function so it can be asserted without opening a device;
// playback_test does exactly that.
// ============================================================================
namespace twmonitor {

// How many channels to pull from the engine for monitoring: 1 or 2, never more.
inline std::size_t pullChannels( std::size_t projectWidth )
{
    return ( projectWidth >= 2 ) ? 2u : 1u;
}

// `src` holds pullChannels(projectWidth) planar buffers of `frames` frames.
// Writes frames * deviceChannels interleaved floats into `out`.
inline void interleave( float *out, std::size_t frames, unsigned deviceChannels,
                        const float *const *src, std::size_t srcChannels )
{
    if( !out || deviceChannels == 0 || srcChannels == 0 ) return;
    for( std::size_t i = 0; i < frames; ++i ) {
        const float l = src[0][i];
        const float r = ( srcChannels >= 2 ) ? src[1][i] : l;
        for( unsigned c = 0; c < deviceChannels; ++c )
            out[i * deviceChannels + c] = ( c % 2 == 0 ) ? l : r;
    }
}

}  // namespace twmonitor

// THE TWO-LANE MACHINE (proposal 21 L1a, design D5).
//
// twSpeaker used to have ONE state axis, because it had one job: play the
// arrangement. Monitoring an input while the transport is stopped is a SECOND
// producer with its own lifetime, so the machine becomes three independent
// axes:
//
//     device { CLOSED, OPEN } x frozen { IDLE, BUFFERING, PLAYING } x live { OFF, ON }
//
//     out = (frozen == PLAYING ? root : 0) + (live == ON ? ring : 0)
//
// and the transitions are:
//
//   arm while stopped   openLive()   -> device OPEN (callback RUNNING), live ON,
//                                       frozen IDLE. The callback zero-fills the
//                                       frozen half and sums the ring.
//   Play                startOutput()-> the frozen lane ATTACHES to the device
//                                       that is already open: no openDevice, no
//                                       second setRenderCallback, and a NEW
//                                       AudioEngine minted under the running
//                                       callback (the handle is swapped
//                                       atomically; engineMutex_ stays a leaf).
//   Stop                stopOutput() -> the frozen lane returns to IDLE and the
//                                       DEVICE STAYS OPEN while live is ON.
//   disarm all          closeLive()  -> live OFF; the device closes iff the
//                                       frozen lane is IDLE.
//
// WHAT DID NOT CHANGE, deliberately: with live OFF every path above is the one
// that was there before — Play opens the device, the readahead priming still
// defers the backend start (so a capture backend's frame 0 is still the first
// frame of real audio), Stop closes it. The priming gates THE FROZEN LANE'S
// PLAYING and never the ring: a monitored input must be audible immediately,
// and it has no readahead to wait for.
enum class DeviceState {
    CLOSED = 0,
    OPEN   = 1,    // openDevice() succeeded; the callback may be running
};

enum class LiveLaneState {
    OFF = 0,
    ON  = 1,
};

// Playback output state machine (Phase 6b: deferred backend startup until buffer ready)
enum class OutputState {
    STOPPED = 0,    // No output, no engine
    OPENING = 1,    // Device opening, engine creating
    BUFFERING = 2,  // Readahead buffering, callback not started
    PLAYING = 3,    // Audio flowing
    STOPPING = 4    // Shutting down
};

class twSpeaker
    : public twComponent
{
    virtual void reset() override;
private:
    std::unique_ptr<audio::AudioBackend> backend_;
    // CRITICAL: Changed from unique_ptr to shared_ptr to prevent use-after-free.
    // The render callback captures 'this' and must access audioEngine_ safely even after
    // stopOutput() is called. With shared_ptr, stopping the backend stops the callback
    // thread, but local copies of the shared_ptr in flight keep the engine alive.
    // This prevents crashes when stopOutput() is called concurrently with setCycle().
    std::shared_ptr<audio::AudioEngine> audioEngine_;
    // Atomic so isPlaying() is a lock-free read. The check-and-act transitions
    // (start/stop) are serialised by outputMutex_ below — atomicity of the flag
    // alone doesn't make "if (!isPlaying_) return; …flip + drive the backend"
    // safe against a concurrent/re-entrant caller.
    std::atomic<bool> isPlaying_;
    // Engine lifecycle: protects the audioEngine_ *handle* only.
    //
    // LOCK ORDER — engineMutex_ is a LEAF lock. It is never held while acquiring
    // mutex() or taskMutex_, and never held across blocking work. In particular:
    //   - ~AudioEngine joins the readahead thread, so the engine must be destroyed
    //     with no lock held: detach it with engineSnapshot()/releaseEngine() and let
    //     the last shared_ptr die outside the lock.
    //   - backend_ calls (openDevice/startOutput/closeDevice) block on the device and
    //     on the render thread; they must never run under engineMutex_.
    // Taking engineMutex_ inside mutex() and mutex() inside engineMutex_ in different
    // places is what previously made the order inverted (latent AB-BA); the leaf rule
    // removes the inversion by construction.
    //
    // Render callback accesses audioEngine_ via a shared_ptr copy (holds a ref even if
    // audioEngine_ is reset); the backend is always stopped before the handle is cleared.
    mutable std::mutex engineMutex_;

    // Task lifecycle: protects bufferingTask_ creation/joining. Brief hold.
    // Prevents race on std::thread assignment between startOutput() and stopOutput().
    mutable std::mutex taskMutex_;

    // Note: stateMutex_ (inherited from twComponent) protects outputState_ transitions.
    // Each lock scope is brief (no expensive I/O), preventing UI thread freezes.
    std::string outputDeviceId_ = "default";

    // Cycle (loop) playback. When enabled with a valid range, the render
    // callback seamlessly wraps the play cursor from loopEnd_ back to loopStart_
    // (the end frame itself is not played, so the loop is [start, end)). The
    // atomics are written from the UI thread (setCycle) and read on the audio
    // thread, so playback never tears on a mid-buffer toggle.
    std::atomic<bool>     cycleEnabled_{ false };
    std::atomic<offset_t> loopStart_{ 0 };
    std::atomic<offset_t> loopEnd_{ 0 };
    // Output frames produced per input (synth-time) frame consumed; 1.0 when the
    // device opened at the graph rate (resampler is a passthrough). Used to bound
    // a pull so it doesn't overshoot the loop end. Set in startOutput().
    double                rateRatio_ = 1.0;

    // App services (graph root, locator authority). Set once at startup via
    // setPlaybackContext(); read on UI and audio threads (the pointer itself
    // never changes after setup, so no synchronization is needed).
    audio::PlaybackContext *context_ = nullptr;
    // Stage 5: borrowed page scheduler for minted engines (see setPageScheduler).
    CaptureRevalidator *pageScheduler_ = nullptr;

    // Phase 6b: Output state machine (deferred backend startup until buffer ready)
    std::atomic<OutputState> outputState_{OutputState::STOPPED};
    std::thread bufferingTask_;          // Background thread monitoring readahead progress
    std::atomic<bool> bufferingTaskRunning_{false};  // Signal to stop buffering task
    // The project width the render callback last REPORTED (twmonitor rule
    // above). 0 = nothing reported yet, so the first callback of a session
    // always logs. An exchange per callback, a log line only when it changes —
    // the alternative is either a per-callback log or a decision the user
    // cannot discover.
    std::atomic<int> monitorWidthLogged_{0};

    // DIAGNOSTIC, and a PERMANENT one — the OUTPUT twin of the recorder's
    // effective-capture-rate check. The render callback counts the frames the
    // device actually consumes; stopOutput divides by wall clock. If that comes
    // out ~44100 while openDevice reported 48000, the device is draining slower
    // than we assume and 48 k content plays ~8.8 % slow and 1.47 semitones flat.
    // No other log can see it: every `rate diag` line is emitted BEFORE the
    // other stream exists, so a mismatch introduced by the second stream
    // opening is invisible to all of them.
    //
    // It is kept rather than removed with the investigation that prompted it
    // (2026-08-17) because the capture-side twin has already earned its keep
    // twice, it costs ONE relaxed atomic add per callback and one log line per
    // stop, and the failure it detects is otherwise reported by users as
    // "everything sounds slightly wrong" with nothing in the log to confirm it.
    // Counter is bumped on the AUDIO thread (relaxed atomic, nothing else);
    // both timestamps are taken on the control thread.
    std::atomic<std::uint64_t> outFramesDelivered_{0};
    std::chrono::steady_clock::time_point outputStartWall_{};
    // When the DEVICE actually began draining (set in monitorReadaheadBuffer
    // once backend_->startOutput() succeeds). The gap to outputStartWall_ is
    // the readahead priming, which is a DELAY; the rate measured from here on
    // is the device CLOCK. One ratio cannot tell those apart, and they call for
    // completely different fixes.
    std::chrono::steady_clock::time_point outputDeviceStartWall_{};

    // Helper: Background task that waits for readahead buffer, then starts backend output
    void monitorReadaheadBuffer();

    // Read the engine handle under engineMutex_ and return a copy. The caller works on
    // the copy with no lock held; the ref keeps the engine alive even if another thread
    // clears audioEngine_ meanwhile.
    std::shared_ptr<audio::AudioEngine> engineSnapshot() const;

    // Detach audioEngine_ under engineMutex_, then destroy it *outside* the lock
    // (~AudioEngine joins the readahead thread — see the lock-order note above).
    void releaseEngine();

protected:
    // Phase 3: IOVector-based interface (type-safe, page-backed)
    virtual length_t calcOutputTo(IOVector& dest, idx_t idx) override;

public:
    ~twSpeaker();
    twSpeaker(tw303aEnvironment &);

    virtual void createOutputLatches(void) override;

    virtual const char *getInputName(idx_t)  const override { return nullptr; }
    virtual const char *getOutputName(idx_t) const override { return nullptr; }
    // ONE input plug (proposal 36 B5). It was two, and neither carried audio:
    // the speaker's callback reads frozen pages through AudioEngine, which asks
    // the ROOT COMPONENT directly, so the plugs have only ever supplied the
    // WIRE FORMAT — startOutput() reads pInputPlugs_[0]->getFormat().sampleRate
    // to open the device, and the rate diagnostic prints it. Plug 1 was read by
    // nobody at all. It was there because a track used to be N parallel mono
    // wires; since B4 the graph is ONE wire N channels wide (twRewire is
    // single-plug above width 1), so a second plug could not be wired even in
    // principle. Keeping it would have been a port that says "the sink is
    // stereo" on the milestone that stops the sink being stereo.
    virtual idx_t getNInputs()  const override { return 1; }
    virtual idx_t getNOutputs() const override { return 0; }

    void setBufferSize(length_t) override {}

    bool isPlaying();

    // Enable/disable cycle (loop) playback and set its bounds (sample frames in
    // synth time). An empty/inverted range (end <= start) disables cycling. Safe
    // to call at any time, including during playback.
    void setCycle( bool enabled, offset_t startFrame, offset_t endFrame );

    // Reposition the playhead WHILE playing (click-to-seek / scrub). Forwards to
    // the live AudioEngine (RT-safe, adopted on the next pull); a no-op when not
    // playing — playback start does its own seek from the locator. Safe to call
    // from the UI thread at any time.
    void requestSeek( offset_t frame );

    // Output device selection (for a device-picker UI). The id is a backend
    // device id from outputDevices(); "default" / empty means the system
    // default endpoint. Takes effect on the next startOutput().
    void setOutputDevice( const std::string &id );
    const std::string &outputDevice() const { return outputDeviceId_; }
    std::vector<audio::AudioDeviceInfo> outputDevices() const;

    // Get the audio backend for querying configuration, latency, and buffer sizes.
    audio::AudioBackend *getBackend() const { return backend_.get(); }

    // Get the audio engine for readahead control and playback state management
    audio::AudioEngine *getAudioEngine() const { return audioEngine_.get(); }

    // Get current output state (for UI status line display)
    OutputState getOutputState() const { return outputState_.load(std::memory_order_relaxed); }

    // App services (graph root, locator). Set once at startup, before any
    // startOutput(); the context must outlive this speaker. See
    // audio/playback_context.h for the threading contract.
    void setPlaybackContext(audio::PlaybackContext *ctx) { context_ = ctx; }

    // Proposal 19 dataflow stage 5: the page scheduler handed to every
    // AudioEngine this speaker mints (readahead becomes a demand consumer;
    // see AudioEngine::setScheduler). Set per project (the revalidator is
    // project-owned); null keeps the legacy pull. Takes effect on the next
    // startOutput().
    void setPageScheduler(CaptureRevalidator *scheduler) { pageScheduler_ = scheduler; }

public:
    // THE FROZEN LANE. Unchanged in every respect except one: when the device is
    // already OPEN (the live lane opened it), it ATTACHES rather than re-opening
    // — and the engine it mints is published under the running callback.
    //
    // Returns true when playback actually started (or was already
    // starting/running — idempotent calls are not a failure), false when the
    // CONFIGURED device could not be opened. On false, no engine was created
    // and outputState_/isPlaying() are exactly as they were before the call —
    // this is the "fall back to no output" contract callers rely on: nothing
    // is blocked, nothing crashes, playback simply did not start. The caller
    // decides what a failed start MEANS for the UI (see SMainWindow's Play
    // handler and SApplication::setPlaybackRunning).
    bool startOutput();
    // Stops the FROZEN LANE. Closes the device only when the live lane is OFF.
    void stopOutput();

    // Non-invasive check: open the CONFIGURED output device and immediately
    // close it again, touching none of the state machine above (no
    // deviceState_/outputState_ transition, no callback registration, no
    // engine). Mirrors the Options dialog's input-device probe pattern
    // (loadAudioPage()). Meant for an interactive startup check — "will the
    // device Play is about to use actually open?" — run BEFORE the user ever
    // presses Play, so a stale/unplugged configured device can be reported
    // once, up front, rather than only discovered on first use. A real
    // startOutput() afterwards makes its own attempt exactly as before; this
    // never opens the device for real.
    //
    // Returns true immediately (no I/O) when the device is already open
    // (a live lane or a previous Play already proved it works).
    bool probeOutputDevice();

    // --- the live lane (proposal 21 L1a, design D5) -------------------------

    // Open the device for live monitoring, at `rate` (the project rate), and
    // turn the live lane ON. Idempotent. Returns 0 on success, -1 on refusal.
    //
    // Unlike startOutput() this starts the backend IMMEDIATELY: there is no
    // readahead to prime, and a performer must hear their input the moment they
    // arm. `channels` is the project width the ring will carry.
    //
    // IT REFUSES WHEN THE DEVICE RATE IS NOT THE PROJECT RATE (review fix 3).
    // The live lane stamps its entries in PROJECT frames and the RT sums them
    // straight into the device buffer, so the two only line up while the rates
    // are equal. The frozen lane has a resampler at this seam; the live lane
    // has nowhere to put one, because a ring entry has to carry a position and
    // a resampler makes the frame count fractional. Refusing loudly beats
    // monitoring at the wrong pitch: one log line naming both rates, a counter,
    // and the device closed again iff the frozen lane is not using it.
    // Resolution path (playback/CONTRACT.md known debt): a device-frame-stamped
    // ring, or opening the device at the project rate (ASIO, proposal 35).
    int  openLive( std::uint32_t rate, idx_t channels );

    // How many openLive() calls were refused for a rate mismatch. Process-wide
    // per speaker; the app reads it to explain itself to the user.
    std::uint64_t liveRateRefusals() const
    { return liveRateRefusals_.load( std::memory_order_relaxed ); }

    // Turn the live lane OFF and close the device if the frozen lane is idle.
    // The PUMP must already be stopped: this does not own it (the app does),
    // and closing the device under a running producer would leave it writing
    // into a ring nobody drains.
    void closeLive();

    bool liveActive() const
    { return liveLane_.load( std::memory_order_acquire ) == LiveLaneState::ON; }
    DeviceState deviceState() const
    { return deviceState_.load( std::memory_order_acquire ); }

    // The pump's producer handle. Valid for the lifetime of the speaker; sized
    // by openLive().
    twLiveMixRing &liveRing() { return liveRing_; }

    // THE LIVE CLOCK (design D2). Stamped by the render callback beside
    // publishPosition(); the pump reads it to decide what position to render.
    const twEngineClock &engineClock() const { return engineClock_; }

    // The arm/disarm crossfade, 2-3 ms (design D2). Applied by the RT sum.
    void setLiveCrossfadeMs( double ms );

    // The device buffer, in PROJECT frames — the publish-lag correction the
    // live clock applies (see twliveclock.h). 0 until a device is open.
    std::uint64_t outputBufferFramesProject() const
    { return bufferFramesProject_.load( std::memory_order_relaxed ); }

private:
    // The ONE render callback, for both lanes. A member rather than a lambda so
    // it can be registered once, at device open, and keep working across a
    // frozen-lane start/stop underneath it.
    std::size_t renderCallbackBody( float *out, std::size_t frames,
                                    std::uint32_t channels );
    // Open the device and register the callback if it is not open already.
    // Returns 0 on success. Never starts the backend — the two lanes disagree
    // about when that should happen and each does it itself.
    int  ensureDeviceOpen( std::uint32_t rate );
    // Close the device. Caller must hold NO lock (closeDevice() waits for the
    // render thread) and must have stopped the backend if it was running.
    void closeDeviceNoLock();

    std::atomic<DeviceState>   deviceState_{ DeviceState::CLOSED };
    std::atomic<bool>          deviceRunning_{ false };   // backend_->startOutput() done
    std::atomic<LiveLaneState> liveLane_{ LiveLaneState::OFF };

    twLiveMixRing  liveRing_;
    // The RT's place in the ring's stream. It MUST live across callbacks: a
    // 480-frame callback leaves the cursor 480 frames into a 1024-frame entry
    // and the next one has to resume there (review fix 1).
    twLiveMixReader liveReader_;
    twLiveMixState liveFade_;          // RT-thread only
    twEngineClock  engineClock_;
    std::atomic<std::uint64_t> bufferFramesProject_{ 0 };
    std::atomic<std::uint32_t> liveCrossfadeFrames_{ 0 };
    std::atomic<std::uint64_t> liveRateRefusals_{ 0 };

    // The callback's planar scratch. Members since proposal 21 L1a: the ring
    // sum needs the pull's PLANAR buffers (it sums before the interleave), and
    // a per-callback std::vector on the RT path was recorded debt anyway. Sized
    // at device open; a callback larger than that grows it once and logs.
    std::vector<float>  cbBuf_;
    std::vector<float*> cbChans_;
    std::atomic<bool>   loggedCbGrow_{ false };
};

#endif
