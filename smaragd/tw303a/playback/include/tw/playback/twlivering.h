#ifndef _TW_LIVE_RING_H_
#define _TW_LIVE_RING_H_

#include "tw/core/twtypes.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

// THE LIVE MIX RING and THE RT SUM (proposal 21 L1a, design D1/D2/D3).
//
// The pump produces one entry per device block; the RT callback consumes it and
// ADDS it to the frozen root page it was going to play anyway:
//
//     out = (frozen == PLAYING ? root : 0) + (live == ON ? ring : 0)
//
// which is sample-for-sample equal to rendering everything together ONLY
// because the master is a unity sum with an identity map — the precondition
// twlive::checkMasterShape() checks and the plan carries as `masterLinear`.
// Where it does not hold, the master joins the closure, the pump renders it,
// and the RT POPS THE RING ONLY (design D3's general rule). Both modes read the
// same entries through the same function below; they differ in whether the
// caller passes a root page at all.
//
// WHY THE ENTRIES ARE POSITION-STAMPED, AND WHY THEY ARE A STREAM.
//
// The pump and the callback are two free-running threads with two different
// clocks. An unstamped queue would happily hand the callback a block rendered
// for a position the playhead has since left (a seek, a loop wrap, or simply
// the pump running ahead), and the mix would be audibly wrong in a way no
// counter could see.
//
// But the stamp must be read as a RANGE, never as an equality. The RT's block
// is VARIABLE on a real device -- WASAPI asks the callback for
// `bufferFrames - padding` frames, so its grid is irregular by construction
// (design F6) -- while the pump produces fixed-size blocks. An RT that summed
// an entry only when `entry.startPos == theFrameBeingDelivered` would align
// with the pump exactly never on real hardware: every entry a mismatch, the
// live lane permanently silent, and a counter saying so in a log nobody reads.
//
// So the ring is consumed as a STREAM OF FRAMES with a read cursor into the
// head entry (twLiveMixRing::Reader). For a wanted range [P, P+n):
//
//   * entries lying wholly BEHIND P are dropped and counted (`dropped`) -- the
//     RT has moved past them, which is what a seek forward or a late callback
//     looks like;
//   * an entry starting AFTER the current want position is the FUTURE and is
//     KEPT: the RT emits silence for the gap and counts `notYet`. Popping it
//     would throw away audio the very next callback needs;
//   * an overlap is summed from the entry's own offset, the cursor advances,
//     and the entry is popped only when it is exhausted -- so one 2048-frame
//     callback consumes two 1024-frame entries and a 33-frame one consumes a
//     sliver of one.
//
// The epoch gate below is evaluated PER ENTRY (a partially consumed entry
// keeps the verdict its epochs give it), and the crossfade state carries
// across entries and across callbacks.
//
// AND THE RUN ID, which is what makes "keep the future" safe. A REPOSITION
// abandons a timeline: everything queued describes a future that no longer
// exists, and it is arbitrarily far from where the RT now is, so the consumer's
// keep-the-future rule would hold it forever -- the ring fills, the producer
// can never write the NEW position, and the live lane stops dead. (Measured:
// exactly that, on a seek back and on the STOPPED -> PLAYING transport switch.)
// So the producer stamps a monotone run id, bumps it at every reposition, and
// publishes it; the consumer DROPS any entry that is not of the current run.
// The ring unblocks within one callback, and the drop is counted rather than
// silent.
//
// WHY THEY ARE EPOCH-TAGGED. Arming a track re-wires the mixer, so the root
// page STILL CONTAINS that track's audio until the re-summed page lands (design
// F4). Summing the ring onto such a page would double the track for as long as
// the re-freeze takes. So an entry carries `flipEpoch` — the ROOT REWIRE's
// contentEpochNow() read right after the exclusion wiring's bump — and the RT
// sums only once the page it is serving is at least that new. Disarm is the
// mirror: `flipEpochPrime` is the root epoch after the re-wiring back, and the
// RT KEEPS summing while the served page is older than it (that page still
// LACKS the track) and stops the moment the re-summed page arrives. A 2-3 ms
// crossfade smooths both flips.
//
// Threading: strict SPSC. ONE producer (the pump) calls beginWrite()/commit();
// ONE consumer (the RT callback) drives a Reader. reset() is a control-plane
// call, legal only while neither side runs. No lock, no allocation on either
// path; the storage is allocated once by reset(). The Reader's cursor is
// CONSUMER-PRIVATE state and must live across callbacks -- twSpeaker holds one
// as a member for exactly that reason.

// The ring depth openLive() uses and twLivePlan::requiredRingDepth() compares
// against. Four covers the default two-block lead (2 inside the lead, 1 being
// consumed, 1 being written); deeper buys latency, not safety.
inline constexpr std::uint32_t twLiveMixRingDefaultDepth = 4;

// One block the pump produced, as the RT sees it.
struct twLiveRingEntry {
    std::int64_t  startPos       = -1;   // PROJECT frame of the block's frame 0
    std::uint64_t runId          = 0;    // which contiguous run produced it
    std::uint64_t flipEpoch      = 0;    // arm side; 0 == no arm gate
    std::uint64_t flipEpochPrime = 0;    // disarm side; 0 == not disarming
    std::uint32_t frames         = 0;
    std::uint32_t channels       = 0;
    bool          playing        = false;  // false == stopped-and-monitoring
    const float  *data           = nullptr;  // planar, `channels * stride`
    std::size_t   stride         = 0;        // floats between channels

    const float *channel( std::size_t c ) const
    {
        if( !data || channels == 0 ) return nullptr;
        if( c >= channels ) c = channels - 1;   // "mono plays on every channel"
        return data + c * stride;
    }
};

// Why a block was or was not summed. Returned by twlive::mixRing so the caller
// owns the counters (the pure function stays pure and unit-testable).
enum class twLiveMixOutcome {
    Summed = 0,
    NoEntry,             // the ring was empty this block
    PositionMismatch,    // the entry describes another frame (one-entry primitive)
    EpochNotYetFlipped,  // arm side: the root page still contains the track
    EpochResummed,       // disarm side: the root page has the track back
};

// What one mixStream() call did. Per CALL, not cumulative -- the ring keeps the
// cumulative numbers, and separating them is what lets a test assert "this
// block dropped exactly one entry" instead of a running total.
struct twLiveStreamStats {
    std::uint32_t dropped      = 0;  // entries wholly behind the wanted range
    std::uint32_t notYet       = 0;  // gaps where the head entry is still future
    std::uint32_t gated        = 0;  // entries the epoch gate refused
    std::uint32_t summed       = 0;  // entries (or parts of them) summed
    std::uint32_t starved      = 0;  // the ring ran empty inside the range
    std::uint32_t framesSummed = 0;
    std::uint32_t framesSilent = 0;  // gap + starvation frames
};

// The crossfade the RT carries ACROSS callbacks. Kept by the caller rather than
// inside the pure function so the function has no state at all.
struct twLiveMixState {
    std::uint32_t fadeFrames = 0;   // ramp length (2-3 ms at the device rate)
    std::uint32_t fadeDone   = 0;   // frames of the ramp already applied
    bool          fadingOut  = false;
};

// THE CONSUMER'S CURSOR. Consumer-private state, deliberately NOT inside the
// ring: the ring is the shared SPSC object and this is one reader's place in
// it. It must live across callbacks, so twSpeaker holds one as a member.
struct twLiveMixReader {
    std::uint32_t cursor = 0;   // frames already consumed from the HEAD entry
    void rewind() { cursor = 0; }
};

// What the RT knows about the frozen lane this block.
struct twLiveMixGate {
    std::int64_t  wantPos   = 0;      // the PROJECT frame being delivered
    std::uint64_t rootEpoch = 0;      // contentEpoch of the root page served
    bool          haveRoot  = false;  // false while STOPPED: out = ring alone
};

class twLiveMixRing;

namespace twlive {

// THE RT SUM, as a pure function — extracted for exactly the reason
// twmonitor::pullChannels/interleave are: the gate is the part of this design
// most able to be got subtly wrong, and it must be assertable without a device,
// a pump, or a graph.
//
// Adds `entry` into the `outChannels` PLANAR buffers (which already hold the
// frozen lane's audio, or silence) for `frames` frames, applying the crossfade
// in `st`. Returns why it did or did not.
//
// It never reads more than `entry.frames` and never writes more than `frames`;
// a channel the entry does not have is served by its last one (the §4.4 read
// clamp), so a mono live lane is heard on every destination channel.
twLiveMixOutcome mixRing( float *const *out, std::size_t outChannels,
                          std::size_t frames, const twLiveRingEntry &entry,
                          const twLiveMixGate &gate, twLiveMixState &st );

// Whether the EPOCHS alone would let this entry be summed (design D2's flip).
// Split out so the decision can be asserted with no buffers at all, and so the
// stream consumer can ask it per entry without a position claim.
twLiveMixOutcome gateEpoch( const twLiveRingEntry &entry, const twLiveMixGate &gate );

// gateEpoch() plus the one-entry primitive's EXACT position claim. Used by
// mixRing(); the stream consumer does not, because the RT's block grid and the
// pump's are different grids by construction (see the header note).
twLiveMixOutcome gateEntry( const twLiveRingEntry &entry, const twLiveMixGate &gate );

// THE RT CONSUMER (proposal 21 L1a review fix 1). Sums the ring's stream over
// [wantPos, wantPos + frames) into `out`, which already holds the frozen lane's
// audio (or silence). Frames the stream does not cover are LEFT ALONE, so the
// caller's zeroing is what makes a gap silence.
//
// It is tolerant of `frames` differing from the ring's framesPerEntry IN BOTH
// DIRECTIONS: a callback smaller than one entry consumes a sliver and leaves
// the cursor mid-entry; a callback larger than one entry consumes several.
//
// Pure in everything except the two pieces of state it is explicitly handed:
// the reader's cursor and the crossfade. That is what keeps it testable
// against a synthetic ring with no device, no pump and no graph.
void mixStream( float *const *out, std::size_t outChannels, std::size_t frames,
                std::int64_t wantPos, const twLiveMixGate &gate,
                twLiveMixRing &ring, twLiveMixReader &reader,
                twLiveMixState &st, twLiveStreamStats &stats );

}  // namespace twlive

class twLiveMixRing {
public:
    // 3-4 deep (design D1). Deeper buys latency, not safety.
    static constexpr std::uint32_t kDefaultDepth = twLiveMixRingDefaultDepth;

    // Control plane. Allocates once; every later call on either side is
    // allocation-free.
    void reset( std::uint32_t channels, std::uint32_t framesPerEntry,
                std::uint32_t depth = kDefaultDepth );

    std::uint32_t channels()       const { return channels_; }
    std::uint32_t framesPerEntry() const { return frames_; }
    std::uint32_t depth()          const { return depth_; }
    bool          configured()     const { return depth_ > 0; }

    // --- producer (the pump) ------------------------------------------------

    // The planar block to write into (`channels * framesPerEntry` floats,
    // channel c at `+ c * framesPerEntry`), or null when the ring is full —
    // which means the RT is not draining, and the pump must DROP rather than
    // block or grow.
    float *beginWrite();

    // Publish what beginWrite() handed out. `frames` may be shorter than
    // framesPerEntry (a short last block); the rest is not read. The entry is
    // stamped with the CURRENT RUN (see setRun).
    void commit( std::int64_t startPos, std::uint32_t frames,
                 std::uint64_t flipEpoch, std::uint64_t flipEpochPrime,
                 bool playing );

    // Producer control plane, called at every REPOSITION and before any commit
    // of the new run. Entries of an older run are dropped by the consumer on
    // its next call, which is what stops an abandoned timeline from filling the
    // ring and starving the producer forever.
    void setRun( std::uint64_t runId )
    { run_.store( runId, std::memory_order_release ); }
    std::uint64_t currentRun() const
    { return run_.load( std::memory_order_acquire ); }

    // --- consumer (the RT callback, through twlive::mixStream) -------------

    // The oldest unread entry, or false. The pointer stays valid until pop().
    bool peek( twLiveRingEntry &out ) const;
    void pop();

    // How many entries are readable right now. The pacing gate asserts on it
    // (a pump that keeps its lead cannot exceed ceil(lead/block) + 1).
    std::uint32_t pending() const;

    // --- counters (both sides; relaxed) ------------------------------------
    std::uint64_t committed()  const { return committed_.load( std::memory_order_relaxed ); }
    std::uint64_t summed()     const { return summed_.load( std::memory_order_relaxed ); }
    std::uint64_t mismatches() const { return mismatches_.load( std::memory_order_relaxed ); }
    std::uint64_t misses()     const { return misses_.load( std::memory_order_relaxed ); }
    std::uint64_t gated()      const { return gated_.load( std::memory_order_relaxed ); }
    std::uint64_t overruns()   const { return overruns_.load( std::memory_order_relaxed ); }
    // Stream-side (review fix 1).
    std::uint64_t dropped()    const { return dropped_.load( std::memory_order_relaxed ); }
    std::uint64_t notYet()     const { return notYet_.load( std::memory_order_relaxed ); }
    // FRAMES OF LIVE AUDIO THE RT ACTUALLY HANDED THE DEVICE, cumulative
    // (proposal 21 L5). It is the only wall-clock-free measure of how far a
    // STOPPED live lane has got: while stopped there is no engine clock and no
    // root page, so the ring IS the transport, and a count-in that ends after
    // N bars of PUMP time has to count exactly this. A QTimer would measure the
    // scheduler instead - 15.6 ms of granularity on Windows, against a beat
    // grid the same case asserts to the frame.
    std::uint64_t framesDelivered() const
    { return delivered_.load( std::memory_order_relaxed ); }
    void noteOutcome( twLiveMixOutcome o );
    void noteStream( const twLiveStreamStats &s );
    void resetStats();

private:
    struct Slot {
        std::int64_t  startPos       = -1;
        std::uint64_t runId          = 0;
        std::uint64_t flipEpoch      = 0;
        std::uint64_t flipEpochPrime = 0;
        std::uint32_t frames         = 0;
        bool          playing        = false;
    };

    std::vector<float> buf_;
    std::vector<Slot>  slots_;
    std::uint32_t      channels_ = 0;
    std::uint32_t      frames_   = 0;
    std::uint32_t      depth_    = 0;

    std::atomic<std::uint64_t> run_{ 0 };    // producer's current run
    std::atomic<std::uint64_t> head_{ 0 };   // entries written (producer)
    std::atomic<std::uint64_t> tail_{ 0 };   // entries read (consumer)

    std::atomic<std::uint64_t> committed_{ 0 };
    std::atomic<std::uint64_t> summed_{ 0 };
    std::atomic<std::uint64_t> mismatches_{ 0 };
    std::atomic<std::uint64_t> misses_{ 0 };
    std::atomic<std::uint64_t> gated_{ 0 };
    std::atomic<std::uint64_t> overruns_{ 0 };
    std::atomic<std::uint64_t> dropped_{ 0 };
    std::atomic<std::uint64_t> notYet_{ 0 };
    std::atomic<std::uint64_t> delivered_{ 0 };
};

#endif  // _TW_LIVE_RING_H_
