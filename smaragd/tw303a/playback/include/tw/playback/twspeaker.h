#ifndef _TWSPEAKER_H_
#define _TWSPEAKER_H_

#include "tw/graph/twcomponent.h"
#include "tw/sources/twresampler.h"
#include "tw/devices/audio_backend.h"
#include "tw/playback/audio_engine.h"
#include "tw/playback/playback_context.h"

#include <atomic>
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
    void startOutput();
    void stopOutput();
};

#endif
