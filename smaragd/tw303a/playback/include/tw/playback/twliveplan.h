#ifndef _TW_LIVE_PLAN_H_
#define _TW_LIVE_PLAN_H_

#include "tw/core/twtypes.h"
#include "tw/mix/twgainstage.h"
#include "tw/plugins/twpluginslotproc.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class twComponent;
class twMixer;
class twRewire;

// THE LIVE PLAN (proposal 21 L1a, design D1).
//
// An IMMUTABLE, main-thread-built snapshot of the live-owned subgraph. It is
// deliberately NOT a component graph: the frozen-page machinery is unreachable
// from the pump (a live thread is `RenderPolicy::Never`), and the parts of the
// graph that survive block-wise are three pure-in-position pieces —
// `twPluginSlotProcessor::render(..., positional=true)`, `twGainStage::
// applyGain` over an Envelope snapshot, and `twRewire`'s channel map. The plan
// is those three, per track, in order, plus the frozen inputs a folder sums by
// position.
//
// LIFETIME. The pump holds it as `shared_ptr<const twLivePlan>` and adopts a
// new one with a single atomic_store; the OLD plan is released after the pump's
// next block, so nothing is destroyed underneath a render in flight (design §3).
//
// SCRATCH. The plan owns the pump's working buffers, allocated ONCE by
// finalize() for the widest track x the device block. They are `mutable` and
// belong to whichever pump holds the plan — which is exactly one, because a
// plan is built for one pump. That is what makes the pump's steady state
// allocation-free without giving the pump a resize path on its own hot loop.

// Where a live track's audio comes in. L1a defines the seam; L1b hands it an
// input device's SPSC ring, L2 an instrument (no audio input at all).
class twLiveInputSource {
public:
    virtual ~twLiveInputSource() = default;

    // Fill `channels` PLANAR buffers with `frames` frames for project position
    // `pos`. Returns how many frames it actually delivered; the pump zero-fills
    // the shortfall (a dropout is silence plus a counter, never a stall).
    //
    // Called ON THE PUMP THREAD: it must not block, allocate, take a component
    // mutex or touch Qt. `pos` is passed because a SYNTHETIC source (the L1a
    // harness) is a pure function of position; a device ring ignores it.
    virtual std::size_t pull( float *const *out, std::size_t channels,
                              std::size_t frames, offset_t pos ) = 0;
};

// One member of the live closure, in render order (children before parents).
struct twLiveTrackPlan {
    // Identity/diagnostics only — the plan NEVER walks the graph. The pump
    // reads pages off `frozenInputs`, and nothing else here is a component.
    std::string name;

    idx_t channels = 1;

    // The slot processors, in slot order. A null entry is skipped (a slot with
    // no processor is transparent), which is how a chain keeps its shape while
    // a plugin is missing.
    //
    // NOT named `slots`: Qt Core is on this module's include path and `slots`
    // is one of its keyword macros, so the member would expand to nothing.
    std::vector<std::shared_ptr<audio::twPluginSlotProcessor> > inserts;

    // The fader, snapshotted when the plan was built (twGainStage::envelope()).
    twGainStage::Envelope gain;

    // twRewire::channelMap(): output channel c is input channel map[c]. Empty
    // is the identity, which is every track today.
    std::vector<idx_t> channelMap;

    // Where this track's audio comes from. Exactly one of:
    //   input      — an armed audio track (case (i)): the input ring.
    //   (neither)  — an instrument track (case (ii)): its slot 0 is a generator
    //                and produces from events; the audio input is silence.
    //   frozen/live — a folder (case (iii)): the sum of its children.
    std::shared_ptr<twLiveInputSource> input;

    // A folder's UNARMED children, read BY POSITION out of their frozen root
    // pages (`getPageIfExists`, try-lock, miss = the previous page or silence).
    // The pump never demands them; the readahead re-roots demands for exactly
    // this list (L1b).
    std::vector<std::shared_ptr<twComponent> > frozenInputs;

    // A folder's LIVE children: indices into twLivePlan::tracks, all strictly
    // less than this track's own index (the vector is in topological order).
    std::vector<int> liveChildren;
};

class twLivePlan {
public:
    std::vector<twLiveTrackPlan> tracks;

    // The top of the closure — the track whose output goes into the ring.
    // -1 == an empty plan (the pump then produces nothing).
    int outputTrack = -1;

    // The device block the plan is scratch-sized for, and the project rate the
    // processors are rendered at.
    length_t blockFrames = 0;
    int      sampleRate  = 48000;

    // THE PRECONDITION RESULT (design D3). true == the master really is
    // `twMixer(unity) -> twRewire(identity)`, so `root(unarmed) + ring` is
    // exact and the RT adds the ring to the frozen root page. false == the
    // master has joined the closure, the pump renders it, and the RT POPS THE
    // RING ONLY. The plan builder decides once; the RT never re-derives it.
    bool masterLinear = true;

    // The epoch gate the entries carry (see twlivering.h). `flipEpoch` is the
    // root rewire's contentEpochNow() read after the arm wiring's bump;
    // `flipEpochPrime` is the same after the disarm wiring, and is 0 for any
    // plan that is not the disarm tail.
    std::uint64_t flipEpoch      = 0;
    std::uint64_t flipEpochPrime = 0;

    // The transport every live-owned processor is rendered under (design D2).
    audio::twLiveTransport transport;

    // While STOPPED the pump has no engine clock to follow, so it counts from
    // here: `vpos = stoppedAnchor + blocks * n`, monotone by construction.
    offset_t stoppedAnchor = 0;

    // How far ahead of the delivered frame the pump renders while PLAYING, in
    // frames: the pump must be at least a block ahead or the RT finds nothing
    // stamped for the frame it is on.
    //
    // NEGATIVE means "take the default" (one block). ZERO is a LEGAL EXPLICIT
    // VALUE and means "render exactly the delivered frame" — which is what a
    // synchronous harness driving the clock itself wants, and the distinction
    // has to be expressible or the harness silently renders a block early.
    length_t leadFrames = -1;

    // Allocate the scratch and validate the shape. Called once, on the main
    // thread, before the plan is published. Returns false (and logs) when the
    // plan is inconsistent — a topological order violation, a zero block.
    bool finalize();

    bool empty() const { return outputTrack < 0 || tracks.empty(); }

    // The pump's per-track working buffers: two planar blocks per track, so a
    // chain of inserts can ping-pong (twPluginSlotProcessor::render forbids
    // aliasing in and out). Valid only after finalize().
    float *scratch( int track, int which ) const;   // which = 0 | 1
    std::size_t scratchStride() const { return (std::size_t)blockFrames; }

private:
    mutable std::vector<float> arena_;
    std::vector<std::size_t>   base_;   // per track, index of its buffer 0
};

namespace twlive {

// THE MASTER-SHAPE PRECONDITION (design D3), as an engine helper over the
// master's own components.
//
// The whole "root(unarmed) + ring" optimization rests on ONE algebraic fact:
//
//     master(unarmed u live) == master(unarmed) + master(live)
//
// which holds sample-for-sample iff the master is a UNITY SUM followed by an
// IDENTITY MAP. That is what `SStdMixer` builds today (design F4) — and the day
// somebody puts an insert, a fader curve or a channel re-map on the master, it
// stops holding and the split becomes an audible bug rather than an
// optimization. So it is CHECKED, here, every time a plan is built, and the
// answer selects between the two modes design D3 names.
//
// It deliberately takes the components rather than walking the graph from the
// root: the app knows which twMixer and which twRewire are "the master", the
// engine does not, and a walk would have to guess.
enum class twMasterMode {
    LinearSplit = 0,   // root(unarmed) + ring in the RT
    Closure,           // the master joins the pump's closure; the RT pops only
};

struct twMasterShape {
    twMasterMode mode   = twMasterMode::LinearSplit;
    const char  *reason = "unity sum, identity map";

    bool linear() const { return mode == twMasterMode::LinearSplit; }
};

// `width` is the project's channel count. A null component, a non-unity input
// level, a non-identity channel map or a width disagreement all mean Closure.
twMasterShape checkMasterShape( const twMixer *mixer, const twRewire *root,
                                idx_t width );

}  // namespace twlive

#endif  // _TW_LIVE_PLAN_H_
