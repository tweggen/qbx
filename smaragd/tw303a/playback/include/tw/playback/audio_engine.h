#ifndef AUDIO_ENGINE_H
#define AUDIO_ENGINE_H

#include "tw/core/twtypes.h"
#include <cstdint>
#include <memory>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>

#include "tw/sources/twresampler.h"
#include "tw/schedule/capture_revalidator.h"  // stage 5: demand consumer
#include "tw/playback/twrtquiescence.h"

class twComponent;
class twLatchStreamingOutput;
struct twOutputPage;

namespace audio {

// Playback state for Phase 6b: minimum buffering before playback starts
enum class PlaybackState {
    STOPPED = 0,    // Not playing
    BUFFERING = 1,  // Readahead building initial buffer (3sec)
    PLAYING = 2     // Sufficient buffer; audio flowing
};


/**
 * AudioEngine: Unified audio pull from component graph with sequential frozen pages.
 *
 * Tier 1 Enhancement: Playback Path Refactoring
 * Uses sequential freezePage() rendering (like export) instead of seekTo(),
 * preserving component state during playback looping.
 *
 * Solves: Reverb/delay state loss during playback (correctness issue)
 *
 * Replaces duplicated pull logic in twspeaker.cc and render_session.cc.
 * Handles:
 * - Component graph traversal with frozen pages (no seekTo corruption)
 * - Position management (playhead tracking, loop cycling)
 * - Channel wiring: N planar output buffers, one per PROJECT channel
 * - Loop cycling with state preservation
 *
 * WIDTH (proposal 36 B5). pullBlock() takes N destination buffers, not
 * (outL, outR): the width of a pull is the width of the page the graph froze,
 * and the caller decides how that maps onto its device (twSpeaker owns that
 * rule, exactly as it already owns the rate mismatch). A channel the page does
 * not have is served by the §4.4 clamp — "mono plays on every channel" — so a
 * caller may always ask for as many channels as it has buffers for.
 */
class AudioEngine {
public:
    /**
     * Create an engine for the given synth component.
     *
     * \param synthOutput Component to pull audio from (e.g., rewire root)
     * \param sampleRate  Sample rate for the engine
     */
    AudioEngine(std::shared_ptr<twComponent> synthOutput, uint32_t sampleRate);
    ~AudioEngine();

    /**
     * Pull a block of audio from the component graph into N planar buffers.
     *
     * Safe to call from realtime thread (callback) or render thread. Every
     * buffer is written for the full nFrames on every path, including the miss
     * paths (which zero it) — a caller never has to clear its own scratch.
     *
     * \param outChannels  nChannels pointers, each holding at least nFrames floats
     * \param nChannels    How many destination buffers (>= 1)
     * \param nFrames      Number of frames to pull
     * \return             Number of frames actually produced (0 on error)
     */
    length_t pullBlock(float* const* outChannels, std::size_t nChannels,
                       length_t nFrames);

    /**
     * The width the graph is currently producing — synthOutput_'s declared
     * channel count. A caller sizes its buffers from this; it may still ask
     * pullBlock() for more or fewer (see the §4.4 clamp above).
     */
    std::size_t graphChannels() const;

    /**
     * Seek to an absolute position in the component graph.
     *
     * Called before rendering a time range, or (before the readahead thread is
     * running) at playback start. Resets component state and the readahead
     * frontier, so it MUST NOT be called while startReadahead() is active — it
     * would race the readahead/RT threads. For a live seek during playback use
     * requestSeek() instead.
     *
     * \param offsetSamples  Absolute position in samples
     */
    void seekTo(uint64_t offsetSamples);

    /**
     * Request a live reposition WHILE playing (scrubbing / click-to-seek).
     *
     * Realtime-safe and callable from the UI thread: it only posts the target
     * position. The RT pull thread adopts it at the top of the next pullBlock()
     * — the sole writer of currentPos_ and the frozen-page cursor, so there is
     * no data race — and the readahead thread notices the playhead jump on its
     * next tick and restarts its chain from there (see readaheadLoop). Playback
     * stays PLAYING (no re-buffering); there may be a brief gap until the target
     * pages are frozen. Does NOT touch the component graph directly; the
     * discontinuity is resolved by freezePage()'s reset()+seekTo() at the first
     * page frozen from the new position.
     *
     * \param offsetSamples  Absolute position in samples
     */
    void requestSeek(uint64_t offsetSamples);

    /**
     * Get the current playback position.
     * Lock-free (safe to call from any thread).
     */
    uint64_t currentPosition() const;

    /**
     * Set loop boundaries and enable/disable looping.
     *
     * Unlike seeking, looping uses sequential frozen pages to preserve state.
     * This ensures reverb/delay/grain components maintain state across loop wraps.
     *
     * \param enabled   Whether to loop
     * \param start     Loop start position (samples)
     * \param end       Loop end position (samples)
     */
    void setLoopBoundaries(bool enabled, uint64_t start, uint64_t end);

    /**
     * Configure the resampler sample rate.
     * Called after device open or when sample rate changes.
     *
     * \param inRate   Input sample rate (component graph rate)
     * \param outRate  Output sample rate (device or render rate)
     */
    void configureResampling(uint32_t inRate, uint32_t outRate);

    /**
     * Start playback after buffering.
     * Phase 6b: Waits for readahead to build minimum 3-second buffer before transitioning to PLAYING.
     * Returns BUFFERING if not ready, PLAYING when buffer threshold met.
     *
     * \return Playback state: BUFFERING (still waiting) or PLAYING (ready)
     */
    /**
     * HOW MUCH READAHEAD `startPlayback()` WAITS FOR before it will let the
     * frozen lane play — published so that a WARM-UP can demand exactly this
     * window and no more.
     *
     * It is the whole reason a transport start from stopped has an audible
     * lead-in on a heavy project: the readahead is created empty at
     * `startOutput()`, so this many frames of the WHOLE graph have to be
     * frozen before one sample is heard. Anything that can freeze them
     * EARLIER (see `twSpeaker::warmFrozenLane`) turns that wait into a walk
     * over cache hits, because the readahead probes with `getPageIfExists`
     * and advances its frontier without re-freezing a page that is current.
     */
    static constexpr uint64_t primingFrames() { return minBufferFrames_; }

    PlaybackState startPlayback();

    /**
     * Get current playback state.
     * \return Current state: STOPPED, BUFFERING, or PLAYING
     */
    PlaybackState getPlaybackState() const;

    /**
     * Get the condition variable that signals when playback is ready.
     * Used by twSpeaker to wait until readahead has buffered enough.
     * \return Reference to playbackReadyCv
     */
    std::condition_variable& getPlaybackReadyCv() { return playbackReadyCv_; }

    // Read-ahead thread management
    void startReadahead();   // Start pre-computing pages in background
    void stopReadahead();    // Stop read-ahead thread

    // Proposal 19 dataflow stage 5 — readahead as a DEMAND CONSUMER. When a
    // scheduler is set (before startReadahead()), the readahead thread no
    // longer executes freezes: on the first page of its window that is not
    // frozen-and-current it issues ONE demand for the remaining contiguous
    // window (pages predecessor-chained within the demand) and re-checks on
    // the next tick — it never blocks and never renders. Already-valid pages
    // are never demanded (a re-demand would re-render non-caching components
    // out of position order — the stage-4 lesson). The RT callback path is
    // untouched: cache reads + the proposal-16 stale fallback. Null (default)
    // keeps the legacy synchronous requestPage pull.
    void setScheduler(CaptureRevalidator *scheduler) { scheduler_ = scheduler; }

    /**
     * The content epoch of the ROOT PAGE the last pull actually served, or 0
     * when it served none (a miss, or the engine is not PLAYING).
     *
     * Added by proposal 21 L1a for the EPOCH-GATED FLIP (design D2). The RT
     * callback sums a live-lane ring entry onto the root page only once that
     * page is at least as new as the arm's `flipEpoch` — because a stale root
     * page STILL CONTAINS the newly armed track's audio and summing would
     * double it. The RT already holds the page; this is simply the number it
     * is holding, published so the sum can be a pure function of it rather
     * than a second lookup.
     *
     * Lock-free; written by the pull thread, read by the same one.
     */
    std::uint64_t servedContentEpoch() const
    { return servedEpoch_.load( std::memory_order_relaxed ); }

    /**
     * RE-ROOT THE FROZEN LANE BELOW THE MASTER CHAIN (proposal 45 M3, AC3.1's
     * engine half; design D4b option 1).
     *
     * The readahead and the RT's frozen-page read normally address
     * `synthOutput_`, which is the mixer's REWIRE -- everything, master chain
     * included. Under a CLOSURE plan that is the two-thread hazard D4b names:
     * freezing a root page runs the master lane's twPluginSlotProcessors on a
     * revalidation worker while the PUMP renders those same instances to
     * produce the master's audio at all. One plugin instance, two threads.
     *
     * Handed the mixer's twMixer (the bus sum, BELOW the master chain), the
     * frozen lane demands and reads pages that stop short of the master, and
     * the pump is the only renderer of the master's processors. `nullptr`
     * restores `synthOutput_`.
     *
     * WHAT THIS DELIBERATELY DOES NOT CHANGE: `graphChannels()` still reports
     * `synthOutput_`'s declared width. That number is the PROJECT's width and
     * feeds twSpeaker's device rule; a re-rooting is about which component's
     * pages are demanded, never about how wide the project is.
     *
     * Main thread only, and it is NOT hot -- it moves exactly when a live plan
     * flips between Linear and Closure. The held page and the readahead
     * frontier belong to the OLD root, so a change drops both through the same
     * seek-pending seam a locator jump uses; without that the RT would go on
     * serving rewire pages while the readahead filled the mixer's.
     *
     * It is called on a RUNNING engine, so the root is PUBLISHED, not assigned
     * (QBX-287; see frozenDemandRoot_ below): the call may wait -- bounded --
     * for the RT pull to finish the one block that may still be reading the old
     * root, so that the old root is never released under it.
     */
    void setFrozenDemandRoot( const std::shared_ptr<twComponent> &root );

    /// The component the FROZEN lane demands and reads, as an owning handle:
    /// `synthOutput_` unless setFrozenDemandRoot installed another. NOT for the
    /// RT thread -- it goes through std::atomic_load, which libstdc++ answers
    /// with a pooled mutex. The pull reads `rtFrozenRoot_` instead (QBX-287).
    std::shared_ptr<twComponent> frozenRoot() const;

    /// Test seam (QBX-287): how many roots setFrozenDemandRoot() could not yet
    /// prove the pull thread had stopped reading, and so still keeps alive.
    /// Main thread only.
    std::size_t retiredRootCountForTest() const { return retiredRoots_.size(); }

    /// QBX-297: how many engines, process-wide, were destroyed on a thread
    /// twRtThreadGuard marks as the RT callback. Must stay 0: ~AudioEngine
    /// joins the readahead thread. twSpeaker's callback never owns an engine
    /// (playback CONTRACT invariant 12), so only a regression can count here.
    static int destroyedOnRtThreadCount()
    { return destroyedOnRtThread_.load(std::memory_order_relaxed); }
    /// QBX-297 test seam: engines destroyed so far, on any thread.
    static int destroyedCount()
    { return destroyed_.load(std::memory_order_relaxed); }

private:
    static std::atomic<int> destroyedOnRtThread_;
    static std::atomic<int> destroyed_;
    std::shared_ptr<twComponent> synthOutput_;
    // HOW THE FROZEN ROOT IS PUBLISHED (QBX-287; playback CONTRACT, "The frozen
    // lane RE-ROOTS"). setFrozenDemandRoot() runs on the main thread while the
    // RT pull and the readahead are running, so the root is read by three
    // threads and written by one:
    //
    //  * frozenDemandRoot_ is the OWNING handle. Null means "synthOutput_",
    //    which is what every path did unconditionally before proposal 45 M3.
    //    Written with std::atomic_store, read with std::atomic_load -- by the
    //    main thread and by the readahead, which takes ONE owning copy per pass.
    //    Neither is the RT, so the lock libstdc++ hides in those calls is fine.
    //  * rtFrozenRoot_ is what the RT pull reads, ONCE per block: a raw pointer
    //    with the synthOutput_ fallback already resolved, so it is non-null
    //    whenever synthOutput_ is. A raw pointer cannot be the last reference,
    //    so the RT never destroys a root and never touches a refcount.
    //  * pullSeq_ is odd while pullBlock() is inside a block. The setter
    //    publishes the new raw pointer and then waits (bounded) for the pull to
    //    leave any block that may have read the old one, BEFORE the old owning
    //    handle is dropped. If the bound runs out the old root is parked in
    //    retiredRoots_ (main thread only) and released at the next setter call
    //    that does observe a quiescent pull, or with the engine.
    std::shared_ptr<twComponent> frozenDemandRoot_;
    std::atomic<twComponent *> rtFrozenRoot_{nullptr};
    twRtQuiescence pullSeq_;   // the handshake (twrtquiescence.h)
    std::vector<std::shared_ptr<twComponent>> retiredRoots_;
    // Pull thread only: the root currentFrozenPage_ was read from. A block that
    // reads a different root drops the held page itself, so no block can serve
    // one root's page under another root's epoch, whatever order it observes
    // the root and the seek seam in.
    twComponent *rtHeldRoot_ = nullptr;
    uint32_t engineSampleRate_;  // The engine's native sample rate

    // Frozen page rendering state (Tier 1 enhancement)
    // Maintains sequential frozen pages for state continuity during playback
    std::shared_ptr<twOutputPage> currentFrozenPage_;
    std::shared_ptr<twOutputPage> prevFrozenPage_;  // For state resumption
    uint64_t currentPageStartPos_;
    size_t pageFrameOffset_;  // Current offset within page's sample buffer
    uint64_t currentPageGeneration_{0};  // Track page generation to detect invalidation
    uint32_t cachedPageValidFrames_{0};  // Phase 2 perf: Cache validFrames to avoid repeated loads

    // Resampling. ONE twResampler PER CHANNEL: the class documents itself as
    // "a single mono channel" and carries per-channel interpolation state, so a
    // shared one would smear the channels together the moment the device rate
    // differs from the project rate (proposal 36 B5). The vector is sized in
    // configureResampling() from the graph's declared width and never resized
    // on the RT path.
    std::vector<twResampler> resamplers_;
    bool passthrough_ = true;         // cached: resamplers_ are all identity
    double rateRatio_;  // output / input sample rate
    // Pre-allocated per-channel resampling buffers (Phase 1 perf optimization),
    // allocated in configureResampling() and reused per block. Flat storage plus
    // a stride, so growing them is one allocation rather than N.
    std::vector<float> resampleBuf_;
    std::size_t resampleStride_ = 0;
    std::size_t resampleChannels_ = 0;
    void ensureResampleBuffers(std::size_t channels, std::size_t frames);
    void resetResamplers();

    // Playback state management (Phase 6b: minimum buffering before playback)
    std::atomic<PlaybackState> playbackState_{PlaybackState::STOPPED};
    static constexpr uint64_t minBufferFrames_ = 144000;       // ~3 seconds at 48kHz; readahead sizes its page window to cover this
    static constexpr uint64_t underrunThresholdFrames_ = 48000; // 1 second at 48kHz (Phase 6b confirmed)

    // Loop state (atomic for lockfree access)
    std::atomic<bool> cycleEnabled_{false};
    std::atomic<uint64_t> loopStart_{0};
    std::atomic<uint64_t> loopEnd_{0};
    std::atomic<uint64_t> currentPos_{0};

    // Live-seek request (requestSeek): posted by the UI thread, adopted by the
    // RT pull thread at the top of pullBlock(). seekPending_ is the handshake;
    // seekTarget_ carries the absolute position.
    std::atomic<bool> seekPending_{false};
    std::atomic<uint64_t> seekTarget_{0};

    // Helper: manage frozen page transitions and state continuity (read-only on
    // audio thread). Both the page choice AND the read cursor (pageFrameOffset_,
    // currentPageStartPos_, cachedPageValidFrames_) are settled here, derived
    // from desiredPos — no caller may carry the cursor across a position jump.

    // `root` is the block's one read of rtFrozenRoot_ (QBX-287).
    void updateFrozenPage(uint64_t desiredPos, twComponent *root);
    // The content epoch of the page updateFrozenPage() ended up holding, or 0.
    std::atomic<uint64_t> servedEpoch_{0};
    // Read-ahead thread: pre-computes pages to keep ahead of playhead
    std::thread readaheadThread_;
    std::atomic<bool> readaheadRunning_{false};
    std::mutex readaheadMutex_;
    std::condition_variable readaheadCv_;
    std::condition_variable playbackReadyCv_;  // Signals twSpeaker when buffer ready for playback
    std::shared_ptr<twOutputPage> readaheadPrevPage_;   // State chain for read-ahead
    uint64_t readaheadComputedUpTo_{0};                 // Last page start pos computed

    // Stage 5: scheduler consumer state (readahead thread only). The pending
    // demand handle stops the 20ms tick from re-issuing an identical demand
    // while its nodes are still in flight; re-issue happens when it completes
    // (or the wanted window/epoch moved on).
    CaptureRevalidator *scheduler_ = nullptr;           // borrowed, optional
    std::shared_ptr<CaptureRevalidator::GraphDemand> pendingDemand_;
    uint64_t pendingDemandStart_{0};
    uint64_t pendingDemandEnd_{0};
    // The content epoch the pending demand was issued AGAINST. Without it the
    // coverage test above is purely positional, so an edit made while a demand
    // is in flight issues no new demand and the pre-edit mix keeps playing until
    // the stale demand happens to finish — up to the whole readahead window
    // (~4 s at 48 kHz). That is why edits and solo/mute clicks read as "ignored"
    // rather than merely late. 0 = no demand issued yet.
    uint64_t pendingDemandEpoch_{0};

    // fix/loop-behaviour, issue h: a SECOND window, demanded/frozen AHEAD OF
    // THE WRAP, so the loop-start pages are already current by the time
    // pullBlock() actually crosses loopEnd_ — before this the whole readahead
    // window was spent on pages PAST loopEnd_ that could never be played, and
    // no page at loopStart_ was ever demanded until the playhead got there.
    // Same shape as the range-A members above, mirrored rather than reused:
    // range A tracks "the rest of THIS pass" and range B tracks "the START of
    // the NEXT pass", and the two chains (state continuity via
    // readaheadPostWrapPrevPage_) must stay independent because the pages are
    // not contiguous with each other.
    std::shared_ptr<twOutputPage> readaheadPostWrapPrevPage_;
    // How far into the loop-start pages (an ABSOLUTE project-frame position,
    // the SAME coordinate space as readaheadComputedUpTo_ — it names a real
    // timeline position, loopStart_ onward, not an offset) THIS pass has
    // pre-fetched ahead of the wrap it is going to hit. 0 = nothing pre-fetched
    // for the upcoming pass yet. Consumed (reset to 0) the instant a wrap is
    // detected and its value is adopted as the new readaheadComputedUpTo_ —
    // see readaheadLoop()'s jump detector — so it always describes "ahead of
    // the wrap that has not happened YET", never a stale prior pass's.
    uint64_t readaheadPostWrapComputedUpTo_{0};
    std::shared_ptr<CaptureRevalidator::GraphDemand> pendingDemand2_;
    uint64_t pendingDemandStart2_{0};
    uint64_t pendingDemandEnd2_{0};
    uint64_t pendingDemandEpoch2_{0};

    void readaheadLoop();  // Entry point for read-ahead thread
};

}  // namespace audio

#endif  // AUDIO_ENGINE_H
