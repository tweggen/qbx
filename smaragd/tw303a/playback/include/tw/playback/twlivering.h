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
// WHY THE ENTRIES ARE POSITION-STAMPED. The pump and the callback are two
// free-running threads with two different clocks. An unstamped queue would
// happily hand the callback a block rendered for a position the playhead has
// since left (a seek, a loop wrap, or simply the pump running one block ahead),
// and the mix would be audibly wrong in a way no counter could see. Stamping
// makes the mismatch a MEASURED DROP instead: the RT sums an entry only when
// its startPos is the frame it is delivering.
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
// ONE consumer (the RT callback) calls peek()/pop(). reset() is a control-plane
// call, legal only while neither side runs. No lock, no allocation on either
// path; the storage is allocated once by reset().

// One block the pump produced, as the RT sees it.
struct twLiveRingEntry {
    std::int64_t  startPos       = -1;   // PROJECT frame of the block's frame 0
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
    PositionMismatch,    // the entry describes another frame
    EpochNotYetFlipped,  // arm side: the root page still contains the track
    EpochResummed,       // disarm side: the root page has the track back
};

// The crossfade the RT carries ACROSS callbacks. Kept by the caller rather than
// inside the pure function so the function has no state at all.
struct twLiveMixState {
    std::uint32_t fadeFrames = 0;   // ramp length (2-3 ms at the device rate)
    std::uint32_t fadeDone   = 0;   // frames of the ramp already applied
    bool          fadingOut  = false;
};

// What the RT knows about the frozen lane this block.
struct twLiveMixGate {
    std::int64_t  wantPos   = 0;      // the PROJECT frame being delivered
    std::uint64_t rootEpoch = 0;      // contentEpoch of the root page served
    bool          haveRoot  = false;  // false while STOPPED: out = ring alone
};

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

// Whether the gate alone (position + epochs) would let this entry be summed.
// Split out so the decision can be asserted with no buffers at all.
twLiveMixOutcome gateEntry( const twLiveRingEntry &entry, const twLiveMixGate &gate );

}  // namespace twlive

class twLiveMixRing {
public:
    // 3-4 deep (design D1). Deeper buys latency, not safety.
    static constexpr std::uint32_t kDefaultDepth = 4;

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
    // framesPerEntry (a short last block); the rest is not read.
    void commit( std::int64_t startPos, std::uint32_t frames,
                 std::uint64_t flipEpoch, std::uint64_t flipEpochPrime,
                 bool playing );

    // --- consumer (the RT callback) ----------------------------------------

    // The oldest unread entry, or false. The pointer stays valid until pop().
    bool peek( twLiveRingEntry &out ) const;
    void pop();

    // Drop every entry whose startPos is BEFORE `pos`. The stale-block policy:
    // a callback that finds an old block discards it and looks at the next one,
    // so a single late block does not desynchronise the whole ring. Returns how
    // many it dropped.
    std::uint32_t dropBefore( std::int64_t pos );

    // --- counters (both sides; relaxed) ------------------------------------
    std::uint64_t committed()  const { return committed_.load( std::memory_order_relaxed ); }
    std::uint64_t summed()     const { return summed_.load( std::memory_order_relaxed ); }
    std::uint64_t mismatches() const { return mismatches_.load( std::memory_order_relaxed ); }
    std::uint64_t misses()     const { return misses_.load( std::memory_order_relaxed ); }
    std::uint64_t gated()      const { return gated_.load( std::memory_order_relaxed ); }
    std::uint64_t overruns()   const { return overruns_.load( std::memory_order_relaxed ); }
    void noteOutcome( twLiveMixOutcome o );
    void resetStats();

private:
    struct Slot {
        std::int64_t  startPos       = -1;
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

    std::atomic<std::uint64_t> head_{ 0 };   // entries written (producer)
    std::atomic<std::uint64_t> tail_{ 0 };   // entries read (consumer)

    std::atomic<std::uint64_t> committed_{ 0 };
    std::atomic<std::uint64_t> summed_{ 0 };
    std::atomic<std::uint64_t> mismatches_{ 0 };
    std::atomic<std::uint64_t> misses_{ 0 };
    std::atomic<std::uint64_t> gated_{ 0 };
    std::atomic<std::uint64_t> overruns_{ 0 };
};

#endif  // _TW_LIVE_RING_H_
