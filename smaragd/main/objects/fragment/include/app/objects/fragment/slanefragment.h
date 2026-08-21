#ifndef _SLANEFRAGMENT_H_
#define _SLANEFRAGMENT_H_

#include <memory>

#include "app/model/sobject.h"
#include "tw/events/tweventclipset.h"

class SProjectLoader;
class QDomElement;
class twTrackMix;

/**
 * SLaneFragment — a path container with NO TRACK IDENTITY (proposal 41 D1).
 *
 * It is an ordered set of clip links and nothing else: no fader, no inserts,
 * no instrument, no solo, no arm, no meters, no automation lanes of its own.
 * It is what SCut windows, exactly as an SCut today windows an STrack or the
 * root SStdMixer (screateassetaction.cpp's "only containers can be windowed"
 * check — proposal 41 D1 extends what an asset may window; it does not add a
 * parallel concept).
 *
 * OWNERSHIP — DECIDED (proposal 41 D2, confirmed by the proposal's author
 * during M1): a fragment is owned by the project-wide ASSET LIST, through the
 * SAME mechanism a container asset already uses. `SProject::registerAsset(
 * name, cut)` (sproject.h/.cpp, unchanged by this milestone) registers a
 * NAMED CUT, exactly as `SCreateAssetAction` does today for a cut windowing
 * an existing STrack/SStdMixer — this class adds nothing there. The fragment
 * itself has NO independent home in the tree: it is constructed with no tree
 * parent (`new SLaneFragment(project)`, exactly the shape
 * `new SMidiSequence(project)` and `new SPlainWave(project)` already use),
 * and its only reference into the live object graph is the unparented
 * content SLink an SCut's constructor builds over it
 * (`SCut(project, *fragment)`). Once M2's `pack-clips` registers that cut
 * under a name, `SProject::registerAsset` pins it with `addRef()` — mixer
 * CONTRACT inv. 2's "removing the last placement does not delete the asset"
 * applies unchanged: `remove-asset` is what drops the cut, and the fragment
 * goes with it because nothing else references it. There is no separate
 * fragment registry, and none should be built — a fragment is exactly as
 * homeless as any other content object (SPlainWave, SMidiSequence) until
 * something windows and names it.
 *
 * Every SObject becomes a QObject child of SProject at construction
 * regardless of tree position (SObject::SObject), which is what makes
 * SProject::serialize()'s flat walk over `this->children()` reach a fragment
 * with no code here needing to know that: the base SObject::serialize()/
 * instantiateFromDomElement machinery, unmodified, is the whole persistence
 * story.
 *
 * D2's "the shared object is the CUT, not the fragment" means exactly one
 * SCut is ever registered as the asset body over a given fragment; multiple
 * placements are SLinks to THAT cut, never a second fragment. SHARING IS THE
 * INVARIANT (D2, amended 2026-08-21): one asset, N placements, edit any and
 * all change — that is the whole point, not a hazard M2 works around. A
 * variation is a NEW ASSET: M2's "Duplicate asset here" gesture deep-copies
 * the FRAGMENT (mints a second `SLaneFragment` with its own children,
 * registers a second cut over it) and repoints ONE placement; the original
 * asset and every other placement of it are untouched, because they were
 * never what was being edited. Nothing in this class's shape blocks that
 * deep copy: a fragment's constructor takes only a project pointer and its
 * children are ordinary SLinks built with the standard
 * `new SLink(content, nullptr); … setParent(fragment)` pattern (see
 * fragmentChildWasAdded), so copying one is "construct a new SLaneFragment,
 * re-link its children's CONTENT objects at the same offsets" — an ordinary
 * walk, not something this milestone's design forecloses.
 *
 * A CONSEQUENCE OF THIS OWNERSHIP, worth stating up front: `objects/fragment`
 * itself gains NO new dependency from it. `SProject::registerAsset` lives in
 * `model`, which this module already depends on, and M1 never calls it —
 * only M2's pack-clips (which registers the fragment's WINDOWING CUT, not the
 * fragment) will, and that verb's natural home is wherever
 * `SCreateAssetAction` already lives (`objects/mixer`, which will need a new
 * `objects/mixer -> objects/fragment` edge at that point). `objects/fragment`
 * itself never needs to see `objects/mixer` or call `registerAsset` — it only
 * needs `SProject`, which it already reaches through `model`.
 *
 * SLaneFragment answers isPathContainer() true (the index-path search may
 * descend into it, and SCut may window it) and isLane() FALSE — the default
 * SObject::isLane() is left untouched (proposal 41 D3). That is the whole
 * point of the M0 split: a fragment must never be consulted for solo, mute,
 * edit-group membership or arm-for-recording, because it carries none of
 * that state, and a fragment-internal flag darkening lanes across the whole
 * project it happens to be placed into would be exactly the kind of accident
 * D3 exists to prevent.
 *
 * M1 SCOPE was: this class holds clip links and sums them at unity through a
 * twTrackMix — nothing else. An event-kind child link was accepted as an
 * ordinary child (it persists and round-trips) but was NOT inserted into the
 * twTrackMix — inserting one as an audio clip entry would ask twTrackMix to
 * freeze a component that does not exist (SObject::getRootComponent()
 * returns null for event content).
 *
 * M3 ADDS the residual event feed (D4): an Event-kind child now goes into
 * ITS OWN twEventClipSet (`eventClips_`, mirroring STrack's own — same
 * key-by-SLink*, same window/note-off/loop machinery, minus the track
 * bubbling a fragment cannot have, D8). resolveEventFeed() flattens that set
 * into ONE immutable snapshot on every call (no dirty-flag cache, matching
 * the "rebuilt on every read" discipline STrack::eventFeed() already uses —
 * a fragment's children are write-once past M2's pack-clips anyway, D3a), so
 * the OUTER SCut windowing this fragment can wrap it with its OWN slip/loop
 * map and, per D5, refuse a non-unity rate rather than double-convert it.
 * M4 adds isPureEventContent() (D7): true while this fragment sums no AUDIO
 * children, so SCut::buildCapture_ skips a UI-thread render that is
 * guaranteed silence.
 */
class SLaneFragment : public SObject
{
    Q_OBJECT
public:
    explicit SLaneFragment( SProject *project );
    ~SLaneFragment() override;

    // Path descent only (proposal 41 D3) — isLane() stays SObject's default
    // (false). A fragment may be windowed and its children path-resolved; it
    // carries no lane state.
    bool isPathContainer() const override { return true; }

    bool hasDuration() const override;
    length_t getDuration() const override;

    // The summing root: ONE twTrackMix at unity, width following the
    // project's channel count (proposal 36 B4 discipline — a fragment's
    // audio children are pages of that width like any other summing parent).
    std::shared_ptr<twComponent> getRootComponent() override;

    // No UI in M1 (Part B, the visual model, is M5+). A fragment is never
    // asked for these directly today — it is windowed by an SCut, which has
    // its own inline renderer — but the base class declares them pure.
    QWidget *getDetailEditWidget( QWidget *parent = nullptr ) override;
    QWidget *getInlineEditWidget( QWidget *parent = nullptr ) override;
    SObjectRenderer *getInlineRenderer() override;

    static SLink *instantiateFromDomElement(
        SProjectLoader &projectLoader, QDomElement &element, SObject *parent );

    // twTrackMix::freezePage mints a fresh page every call — it caches
    // nothing of its own (proposal 34's metering note records this: "always
    // empty" page map) — but its CLIP ENTRIES resolve into each child's own
    // view/streaming-latch, which DOES cache, and invalidatePagesInRange is
    // what tells those to go stale. Without these overrides, an edit that
    // reaches a fragment's CHILD from elsewhere in the tree (the walk in
    // SObject::invalidateRenderPath[Range] bumps every container ON THE PATH,
    // this fragment included) would silently stop doing anything useful the
    // moment it reached here — exactly the STrack/SStdMixer override these
    // mirror, minus the plugin chain / gain stage / rewire a fragment has
    // none of (D1).
    //
    // Proposal 41 M2b: ALSO the seam refreshClipGainCurves() covers on a
    // track. The walk above correctly bumps THIS object's own epoch when a
    // clip nested inside it changes — that part already worked — but bumping
    // the epoch only tells the twTrackMix's PAGES to go stale; it does not,
    // by itself, re-read a child's getVolume()/cut:Gain into the CLIP ENTRY
    // twTrackMix actually applies gain through (ClipEntry::gainScalar /
    // ::gainCurve). STrack::bumpRenderChainEpoch[Range]() re-pulls every
    // direct child's gain for exactly this reason; a fragment is the same
    // kind of twTrackMix owner and needs the same pull, or a clip's volume
    // (or cut:Gain envelope) inside a fragment is audible nowhere, through
    // any placement, no matter how correctly the invalidation walk reaches
    // it.
    void bumpRenderChainEpoch() override;
    void bumpRenderChainEpochRange( offset_t start, offset_t end ) override;

    // Proposal 41 D4/M3: the residual feed of every Event-kind child,
    // flattened into one snapshot. clipPos is unused (see the base class
    // doc) — the returned map is always identity: THIS object's own zero is
    // the flattened sequence's zero, and any slip/loop/channel remap is the
    // WINDOW's job (the SCut that windows this fragment applies its own map
    // on top, exactly as SMidiCut's window sits above SMidiSequence's
    // content-relative table).
    twEventClipResolved resolveEventFeed( offset_t clipPos ) override;

    // Proposal 41 D7/M4: true while this fragment has no AUDIO-content
    // children (empty, or event-only) — its twTrackMix sum is silence, and
    // SCut::buildCapture_ must not render that silence on the UI thread.
    bool isPureEventContent() const override { return audioChildCount_ == 0; }

private slots:
    // Mirrors STrack's trackChildWasAdded/Removed/Moved/DurationChanged
    // (strack.cpp), routing an Event-kind child into eventClips_ and an
    // audio-kind one into cpTrackMix_ (M3) — stripped of what a fragment
    // does not have: no live-recording short-circuit (a fragment cannot be
    // armed), no nested-track mute/solo forwarding (D8 — a fragment is
    // single-lane by construction and holds clips, not lanes).
    void fragmentChildWasAdded( SLink &child );
    void fragmentChildWasRemoved( SLink &child );
    void fragmentChildWasMoved( offset_t newTime );
    void fragmentChildDurationChanged( length_t newLength );

    // The project's channel count reaches every summing parent (proposal 36
    // B4); a fragment is one, so it follows the width exactly as STrack and
    // SStdMixer do. Width-following alone, never plumbed further — a
    // fragment has no plugin chain to re-derive a mismatch mapping for.
    void onProjectChannelsChanged( int n );

private:
    void checkDurationChanged();

    // Mirrors STrack::refreshClipGainCurves() (strack.cpp) — same reasoning,
    // same shape, over THIS fragment's own DIRECT children instead of a
    // track's. Pulls each audio child's static getVolume() (dB, converted to
    // the linear scalar ClipEntry::gainScalar wants — every SObject answers
    // 0 dB by default, so this is a no-op for a clip that never set one) and
    // its cut:Gain automation curve (proposal 41's non-goals: "cut:Gain on a
    // child clip's own window travels with that window, unchanged" — that
    // sentence is only true if something pulls it into the CONTAINER that
    // actually owns the twTrackMix, exactly as it is for a track).
    void refreshClipGainCurves();

    std::shared_ptr<twTrackMix> cpTrackMix_;
    mutable length_t lastDuration_;
    mutable bool lastDurationValid_;

    // Proposal 41 M3: this fragment's OWN event clip set, one entry per
    // Event-kind child link (key = SLink*, exactly as STrack's eventClips_).
    // Gives window gating, synthesised note-offs and loop tiling for every
    // child FOR FREE — the same tested twEventClipSet code SMidiCut's own
    // placement already runs through.
    twEventClipSet eventClips_;

    // Proposal 41 M4: how many of our children are audio-content (inserted
    // into cpTrackMix_). Kept as a counter rather than asking twTrackMix
    // (which has no clip-count accessor) — incremented/decremented exactly
    // where cpTrackMix_->insertClip/removeClip are called below.
    int audioChildCount_ = 0;
};

#endif
