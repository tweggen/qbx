#ifndef _TW_LIVE_PUMP_H_
#define _TW_LIVE_PUMP_H_

#include "tw/playback/twliveclock.h"
#include "tw/playback/twliveplan.h"
#include "tw/playback/twlivering.h"
#include "tw/devices/precise_waiter.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// THE LIVE GRAPH PUMP (proposal 21 L1a, design D1/D2).
//
// One std::thread per active live lane. Once per device block it walks the
// plan, renders every live-owned track through its own processors, sums the
// closure, and pushes ONE position-stamped entry into the live ring. It is the
// only thread allowed to call `render()` on a live-owned processor.
//
// WHAT IT MAY DO (threading contract, design §4): take a live-owned processor's
// own `mutex_` (bounded — that processor is not being rendered by anybody
// else), `getPageIfExists` (try-lock, miss = silence), push the ring, read the
// engine position atomic.
//
// WHAT IT MUST NOT DO: `freezePage` / `requestPage` / `fetchInputPage` /
// `copyData`, `requestGraphPages`, any blocking component `mutex()`, any
// steady-state allocation, anything Qt. `markLiveThread()` (proposal 21 L0)
// enforces the first of those at the one check in `twComponent::freezePage`:
// a freeze reached from here answers silence and counts rather than blocking
// the pump on a disk read.
//
// THE CLOCK, AND THE PACING (design D2; review fix 2).
//
//   PLAYING  the pump keeps [nextFrame, nextFrame + lead) COVERED, where
//            nextFrame is the frame the RT will pull next (twliveclock.h). It
//            renders while `nextPos_ < nextFrame + lead` and the ring has room,
//            and idles otherwise.
//   STOPPED  a virtual counter from plan->stoppedAnchor, paced by the ring
//            drain (there is no clock to follow). Only the LIVE side is
//            rendered: the inputs, the live children and their processors.
//            `frozenInputs` are NOT summed - stopped means nothing sequenced
//            sounds (D2), and the RT outputs `out = ring`, so a frozen input
//            summed here would be the arrangement playing on after Stop.
//
// FILLING THE RING UNTIL IT IS FULL IS WRONG AND WAS THE ORIGINAL BUG. With a
// depth-4 ring and a 2-block tolerance the pump ran 4 blocks ahead, so the very
// next stamp read as a 3-block backwards jump, repositioned, forgot continuity
// and re-rendered the covered range — on every start, forever. Pacing on the
// frames the RT actually wants removes the failure mode rather than widening
// the tolerance past it.
//
// REPOSITION RULES (design D2's "ONE explicit reposition per start / stop /
// seek / wrap"), which replace the old fixed drift tolerance:
//
//   nextPos_ <  nextFrame                    fell behind, or a seek FORWARD
//                                            past the covered range
//   nextPos_ >  nextFrame + lead + block      the clock moved BACK: a seek back
//                                            or a loop wrap
//   requestReposition()                       the app said so explicitly
//
// A jump INSIDE the covered window needs none: the RT drops the entries it has
// passed and streams on. A reposition tells the plan's processors to forget
// continuity, so a generator resets, chases and pre-rolls at the new position.
// The counter is a gate: a contiguous run must produce exactly one (the first),
// and each seek exactly one more.
//
// `renderOneBlock()` is public and SYNCHRONOUS on purpose. The thread loop is a
// pacer around it, and the L1a harness drives it directly — with no pacing, no
// device and no ring consumer — which is the only way a block-wise render can
// be compared to a frozen render sample-for-sample.
class LiveGraphPump {
public:
    LiveGraphPump( twLiveMixRing &ring, const twEngineClock &clock );
    ~LiveGraphPump();

    LiveGraphPump( const LiveGraphPump & )            = delete;
    LiveGraphPump &operator=( const LiveGraphPump & ) = delete;

    // Publish a plan. Lock-free for the pump: it adopts the new one at the top
    // of its next block and drops its reference to the old one there, which is
    // what "the old plan is released after the pump's next block" means.
    void setPlan( std::shared_ptr<const twLivePlan> plan );
    std::shared_ptr<const twLivePlan> plan() const;

    // The thread. start() is idempotent; stop() joins.
    void start();
    void stop();

    // Force ONE reposition at the top of the next block (design D2). The app
    // knows about a start, a stop, a seek and a loop wrap before any clock
    // reading can show them, and relying on drift detection alone would make
    // the first block after a transport action a race. Consumed by the pump;
    // safe from any thread.
    void requestReposition() { repositionReq_.store( true, std::memory_order_release ); }
    bool running() const { return running_.load( std::memory_order_acquire ); }

    // ONE block, on the CALLING thread. Marks the caller as a live thread (so
    // the ownership guard and the render policy behave exactly as they do for
    // the pump thread) and returns false when there is nothing to do — no plan,
    // an empty plan, or a ring that is full.
    bool renderOneBlock();

    // --- counters -----------------------------------------------------------
    std::uint64_t blocks()            const { return blocks_.load( std::memory_order_relaxed ); }
    std::uint64_t repositions()       const { return repositions_.load( std::memory_order_relaxed ); }
    // QBX-214: late wake-ups that were absorbed as a CATCH-UP (a forward jump
    // of at most twPluginSlotProcessor::kLiveCatchUpMaxFrames while playing)
    // rather than a reposition. Counted apart so repositions() still means
    // "the pump abandoned its run", and so a gate can still see the pump being
    // late after lateness stopped costing an instrument its voices.
    std::uint64_t catchUps()          const { return catchUps_.load( std::memory_order_relaxed ); }
    std::uint64_t frozenInputMisses() const { return frozenMisses_.load( std::memory_order_relaxed ); }
    std::uint64_t inputShortfalls()   const { return shortfalls_.load( std::memory_order_relaxed ); }
    std::uint64_t ringFull()          const { return ringFull_.load( std::memory_order_relaxed ); }
    // QBX-320: frames of live INPUT the pump threw away to resync after a
    // clock stall (twEngineClock::noteStall), and how many stalls it saw. A
    // gate pairs the first with the speaker's stall count: equal means every
    // stalled frame was given back, and nothing else was dropped.
    std::uint64_t resyncFrames()      const { return resyncFrames_.load( std::memory_order_relaxed ); }
    std::uint64_t resyncs()           const { return resyncs_.load( std::memory_order_relaxed ); }
    // QBX-323: frames of live INPUT the pump threw away to resync after its
    // own CATCH-UPS - the positions a catch-up skipped, which it never pulled
    // input for. Kept apart from resyncFrames() so "resyncFrames == the
    // speaker's stall count" still means what it says.
    std::uint64_t catchUpResyncFrames() const
    { return catchUpResyncFrames_.load( std::memory_order_relaxed ); }
    // TEST SEAM (QBX-323): make the pump LATE by at least `frames`. The next
    // playing call that has a position stops rendering until the clock's next
    // frame has passed the pump's own position by `frames`, then carries on -
    // which is a catch-up (or, beyond twPluginSlotProcessor::
    // kLiveCatchUpMaxFrames, a reposition) exactly as a descheduled pump
    // meets it. Safe from any thread; one atomic on the pump path.
    void forceLateForTest( std::int64_t frames )
    { lateReqForTest_.store( frames, std::memory_order_release ); }
    // The position the last block was rendered at (diagnostics and tests).
    offset_t      lastPosition()      const { return lastPos_.load( std::memory_order_relaxed ); }
    void resetStats();

private:
    void loop();
    // Apply ONE reposition: move the position, clear the request, count it,
    // open a new RUN on the ring, and tell the plan's processors to forget
    // continuity. Pump thread only.
    void applyReposition( const twLivePlan &plan, offset_t want );
    // Everything below runs on the pump thread (or the harness's caller).
    void renderTrack( const twLivePlan &plan, int index, length_t frames, offset_t pos );
    // Sum one frozen input's page at `pos` into `dst` (planar, `channels` x
    // `frames`). Returns false on a miss — silence for that input this block.
    bool sumFrozenInput( int trackIndex, std::size_t inputIndex,
                         const std::shared_ptr<twComponent> &comp,
                         float *dst, std::size_t stride, idx_t channels,
                         length_t frames, offset_t pos );

    twLiveMixRing       &ring_;
    const twEngineClock &clock_;

    std::shared_ptr<const twLivePlan> plan_;        // guarded by planMutex_
    mutable std::mutex                planMutex_;   // NEVER taken on the pump's
                                                    // hot path: the pump keeps
                                                    // its own reference and
                                                    // re-reads only when the
                                                    // generation moves.
    std::atomic<std::uint64_t> planGen_{ 0 };

    // The pump's own copy of the plan and the position state that goes with it.
    std::shared_ptr<const twLivePlan> held_;
    std::uint64_t                     heldGen_ = 0;
    // One retained page per frozen input of the held plan, flattened
    // [track][input] — "keep the previous page like twLevelProbe" (design D1).
    std::vector<std::shared_ptr<twOutputPage> > heldPages_;
    std::vector<std::size_t>                    pageBase_;
    // Which of a track's two scratch buffers holds its RESULT, per track. The
    // insert chain and the channel map ping-pong, so "the last one written" is
    // not a constant.
    std::vector<int>                            resultBuf_;
    // Pointer arrays handed to render()/pull(). Members so a block allocates
    // nothing once the plan is adopted.
    std::vector<const float *> inPtrs_;
    std::vector<float *>       outPtrs_;

    offset_t      nextPos_   = 0;
    bool          havePos_   = false;
    bool          wasPlaying_ = false;
    std::uint64_t blockIndex_ = 0;     // blocks since the last reposition
    std::uint64_t lastSeq_    = 0;
    std::atomic<bool> repositionReq_{ false };
    // The clock's stall counter as last seen, and the input frames still owed
    // to a resync (QBX-320). Re-anchored at start(), so a stall from before
    // this run of the pump -- a different arm, a different input backlog --
    // is never "repaid" out of the new one.
    std::uint64_t     stallSeen_   = 0;
    bool              stallSynced_ = false;
    std::uint64_t     resyncDebt_  = 0;
    // The positions skipped by catch-ups and not yet repaid out of the input
    // (QBX-323). Paid together with resyncDebt_, after it; re-anchored at
    // start() like it.
    std::uint64_t     catchUpDebt_ = 0;
    // forceLateForTest(): the request, and the clock frame the pump idles
    // until once it has been taken up (< 0 == not holding). Pump thread only.
    std::atomic<std::int64_t> lateReqForTest_{ 0 };
    offset_t          lateUntil_   = -1;
    std::uint64_t     runId_ = 0;
    bool              loggedShallowRing_ = false;

    std::thread       thread_;
    std::atomic<bool> running_{ false };
    std::atomic<bool> stopFlag_{ false };
    // The idle wait (QBX-214). Opened before the thread starts, closed after it
    // joins; wake() from stop() so the join does not wait out a nap.
    audio::PreciseWaiter waiter_;

    std::atomic<std::uint64_t> blocks_{ 0 };
    std::atomic<std::uint64_t> repositions_{ 0 };
    std::atomic<std::uint64_t> catchUps_{ 0 };
    std::atomic<std::uint64_t> frozenMisses_{ 0 };
    std::atomic<std::uint64_t> shortfalls_{ 0 };
    std::atomic<std::uint64_t> ringFull_{ 0 };
    std::atomic<std::uint64_t> resyncFrames_{ 0 };
    std::atomic<std::uint64_t> resyncs_{ 0 };
    std::atomic<std::uint64_t> catchUpResyncFrames_{ 0 };
    std::atomic<offset_t>      lastPos_{ 0 };
};

#endif  // _TW_LIVE_PUMP_H_
