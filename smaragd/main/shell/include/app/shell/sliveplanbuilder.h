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
/**
 * ONE armed MIDI track and the processor its notes actually reach (proposal 21
 * L2, design D4 / section 3 case (iii)).
 *
 * `armed` is the track carrying `trackInput=midi:… | keyboard`; `consumer` is
 * the track whose SLOT 0 is the instrument that will sound it — itself when it
 * has one, otherwise the nearest ancestor it bubbles its events up to. The
 * distinction is the whole of the folder-drum-machine case: the child is a MIDI
 * SOURCE, the folder is the live INSTRUMENT, and it is the FOLDER that leaves
 * the frozen sum.
 */
struct SLiveMidiFeed {
    STrack *armed    = nullptr;
    STrack *consumer = nullptr;
    QString port;              // portable NAME; "keyboard" is one
    int     channel  = -1;     // 0-based; -1 == any

    bool operator==( const SLiveMidiFeed &o ) const
    {
        return armed == o.armed && consumer == o.consumer && port == o.port
               && channel == o.channel;
    }
    bool operator!=( const SLiveMidiFeed &o ) const { return !( *this == o ); }
};

struct SLiveClosure {
    /**
     * THE METRONOME (proposal 21 L5, design D1). It is not a track and owns no
     * track, so it is a FLAG rather than a member: a live lane exists iff
     * `armed u monitor u metronome`, and the click joins the plan at the
     * OUTPUT as a synthetic source with no processors and unity gain.
     *
     * Keeping it out of `ordered` is what leaves the whole arm/disarm protocol
     * untouched - nothing is retired, nothing is live-owned, no plug is nulled
     * - so a metronome-only lane cannot change one byte of what the frozen
     * graph produces. That is the property `metronome_render_identity` gates.
     */
    bool metronome = false;

    /// Every member, children before parents.
    std::vector<STrack *> ordered;
    /// The subset that has a live INPUT (the armed / monitoring tracks).
    std::vector<STrack *> sources;
    /// The MIDI half of `sources` (proposal 21 L2). A feed's `consumer` is in
    /// `sources`; its `armed` track may not be a member at all.
    std::vector<SLiveMidiFeed> midiFeeds;
    /// The members that are direct children of the root mixer — the TOPMOST
    /// ones by construction, which is why the mixer's exclusion rule is simply
    /// "in the closure ⇒ null the plug".
    std::vector<STrack *> topLevel;

    /// No lane at all. The metronome counts: it is the one member of the live
    /// set that is not a track.
    bool empty() const { return ordered.empty() && !metronome; }
    /// Is there anything the INPUT DEVICE is needed for? A metronome-only lane
    /// needs no microphone, and opening one would be a surprise.
    bool needsInput() const { return !sources.empty(); }
    bool contains( const STrack *t ) const;
};

namespace sliveplan {

/**
 * Read the master lane's chain state off a mixer, for `checkMasterShape`
 * (proposal 45 M2 / D4a).
 *
 * ONE spelling, shared by the plan builder and SLiveMonitor. Both ask the same
 * question on every plan build and every refresh; two copies would be two
 * chances for the refusal and the plan to disagree about whether monitoring is
 * legal — and the disagreement would be silent, because the plan is what the
 * RT reads and the refusal is what the user sees.
 */
twlive::twMasterChainState masterChainStateOf( const SStdMixer *mixer );

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
 * Since L2 both halves are live. An `audio:` track is a source in its own
 * right; a `midi:`/`keyboard` track contributes its CONSUMER (design D4) —
 * itself when it holds the instrument, its folder when it bubbles its events
 * up to one. A MIDI track whose notes would reach no instrument at all is
 * still not a source: excluding it would trade the arrangement for silence.
 */
SLiveClosure computeClosure( SObject *rootMixer, bool playing, bool recording,
                             const std::vector<const STrack *> &inertlyArmed );

/**
 * Should the click be in the plan right now (proposal 21 L5, extended by the
 * "Click while recording" menu item)?
 *
 * `metronomeOn` is the project's own `SProjectProps::Metronome`; the click is
 * audible while the transport is PLAYING or RECORDING, which is what every
 * reference DAW does - a metronome ticking at a stopped playhead is a bug
 * report, not a feature. `countIn` overrides both: the count-in IS the stopped
 * transport's live lane, ticking down to the record start (design D2).
 *
 * `clickWhileRecording` is `SOpt::ClickWhileRecording` (per-user, default on):
 * while `recording` is true it is the SOLE authority on whether the ordinary
 * beat click sounds, independent of `playing` (recording implies the
 * transport is running). Unchecking it silences the click for the take
 * without touching the metronome switch itself, so plain playback keeps
 * clicking exactly as before.
 */
bool metronomeWanted( bool metronomeOn, bool playing, bool recording,
                      bool countIn, bool clickWhileRecording );

/// Does this track's `trackInput` name an audio device (the L1b half)?
bool isAudioInput( const STrack *t );
/// Does it name a MIDI port or the computer keyboard (the L2 half)?
bool isMidiInput( const STrack *t );
/**
 * The track whose SLOT 0 will sound `t`'s live notes: `t` itself when it has an
 * instrument, else the nearest ancestor it bubbles its events up to that has
 * one. Null when nothing would sound them — and a track with no consumer is
 * deliberately NOT a live source, because excluding it from the frozen sum
 * would trade the arrangement for silence (design D3's "silence is the worse
 * answer").
 */
STrack *midiConsumerFor( SObject *rootMixer, STrack *t );

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
        /**
         * THE CLICK, as a live source (proposal 21 L5, design D1/D2). Non-null
         * adds ONE synthetic plan track at the output: no inserts, unity gain,
         * identity map, `input` = this. It is never a member of the closure
         * and never live-owns anything, so it cannot perturb the frozen graph.
         *
         * A render never sees it: `SLiveMonitor::suspendForRender()` drops
         * every lane, so there is no branch here for "is this a render".
         */
        std::shared_ptr<twLiveInputSource> metronome;
    };

    static std::shared_ptr<twLivePlan> build( const SLiveClosure &closure,
                                              const Params &params,
                                              const SourceFn &sourceFor );
};

#endif // _SLIVEPLANBUILDER_H_
