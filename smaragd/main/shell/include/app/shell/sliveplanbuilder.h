#ifndef _SLIVEPLANBUILDER_H_
#define _SLIVEPLANBUILDER_H_

#include <functional>
#include <memory>
#include <vector>

#include <QString>

#include "tw/core/twtypes.h"
#include "tw/playback/twliveplan.h"

class SObject;
class SStdMixer;
class STrack;

/**
 * THE LIVE CLOSURE and THE PLAN BUILDER (proposal 21 L1b, design D1/D3/D9).
 *
 * The closure is "every component downstream of a live-owned track up to the
 * OUTPUT": the armed track itself, then every folder above it, up to the
 * master. It is computed by WALKING the model on the main thread — the same
 * argument `sinstruments::collectInstrumentTracks` and `SMidiOutPump::
 * collectTracks` make: a registration list has to be poked from everywhere a
 * track can be reparented, and solo/arm/monitor are global questions anyway.
 *
 * THE ORDER IS PART OF THE CONTRACT. `twLivePlan::finalize()` proves that
 * every `liveChildren` index is strictly less than its parent's, because the
 * pump renders the vector front to back and a parent sums buffers its children
 * have already written. Emitting by DECREASING DEPTH gives that for free.
 */
struct SLiveClosure {
    /// Every member, children before parents.
    std::vector<STrack *> ordered;
    /// The subset that has a live INPUT (the armed / monitoring tracks).
    std::vector<STrack *> sources;
    /// The members that are direct children of the root mixer — the TOPMOST
    /// ones by construction, which is why the mixer's exclusion rule is simply
    /// "in the closure ⇒ null the plug".
    std::vector<STrack *> topLevel;

    bool empty() const { return ordered.empty(); }
    bool contains( const STrack *t ) const;
};

namespace sliveplan {

/**
 * The live set: `{armed && monitorEffective} ∪ {monitorMode == on}`, closed
 * upward to the master (design D9).
 *
 * `inertlyArmed` is the set of tracks whose `ArmedForRecording` came out of a
 * PROJECT FILE and has not been touched in this session. Design D9 says arm
 * "never starts monitoring on load", so those are not sources — a loaded
 * project must not open the developer's microphone — while the flag itself
 * still round-trips through the file untouched.
 *
 * L1b consumes `audio:` inputs only. A `midi:` or `keyboard` input is a
 * perfectly legal spelling that nothing renders yet (L2), so such a track is
 * not a source and is not excluded from the frozen sum: silence would be a
 * worse answer than the arrangement.
 */
SLiveClosure computeClosure( SObject *rootMixer, bool playing, bool recording,
                             const std::vector<const STrack *> &inertlyArmed );

/// Does this track's `trackInput` name an audio device (the L1b half)?
bool isAudioInput( const STrack *t );

}  // namespace sliveplan

/**
 * Turns a closure into an immutable `twLivePlan`. Main thread only; the plan
 * is published to the pump with a single `setPlan()`.
 */
class SLivePlanBuilder
{
public:
    /// Where a source track's audio comes from. Returns null for a track the
    /// caller has no open device for — the plan then renders it as silence
    /// through its own inserts, which is honest and keeps the shape stable.
    using SourceFn = std::function<std::shared_ptr<twLiveInputSource>( STrack * )>;

    struct Params {
        SStdMixer *mixer        = nullptr;
        length_t   blockFrames  = 1024;
        int        sampleRate   = 48000;
        idx_t      width        = 2;
        bool       playing      = false;
        bool       recording    = false;
        offset_t   locator      = 0;
        std::uint64_t flipEpoch      = 0;
        std::uint64_t flipEpochPrime = 0;
        length_t   leadFrames    = -1;   // < 0 = the pump's default (2 blocks)
    };

    static std::shared_ptr<twLivePlan> build( const SLiveClosure &closure,
                                              const Params &params,
                                              const SourceFn &sourceFor );
};

#endif // _SLIVEPLANBUILDER_H_
