#ifndef _SLANEFRAGMENT_H_
#define _SLANEFRAGMENT_H_

#include <memory>

#include "app/model/sobject.h"

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
 * M1 SCOPE. This class holds clip links and sums them at unity through a
 * twTrackMix — nothing else. Residual event-feed bubbling (D4), the pure-
 * event silent-capture short-circuit (D7) and the rate-refusal rule (D5) are
 * proposal 41 M3/M4; pack-clips/unpack-clips and the "duplicate asset here"
 * deep-copy gesture (M2) and the visual model (Part B, M5-M7) are later
 * milestones. An event-kind child link is
 * therefore accepted as an ordinary child (it persists and round-trips) but
 * is deliberately NOT inserted into the twTrackMix — there is nothing yet
 * that consumes an event feed out of a fragment, and inserting one as an
 * audio clip entry would ask twTrackMix to freeze a component that does not
 * exist (SObject::getRootComponent() returns null for event content).
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
    void bumpRenderChainEpoch() override;
    void bumpRenderChainEpochRange( offset_t start, offset_t end ) override;

private slots:
    // Mirrors STrack's trackChildWasAdded/Removed/Moved/DurationChanged
    // (strack.cpp), stripped of everything a fragment does not have: no
    // event clip set (M3), no live-recording short-circuit (a fragment
    // cannot be armed), no nested-track mute/solo forwarding (D8 — a
    // fragment is single-lane by construction and holds clips, not lanes).
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

    std::shared_ptr<twTrackMix> cpTrackMix_;
    mutable length_t lastDuration_;
    mutable bool lastDurationValid_;
};

#endif
