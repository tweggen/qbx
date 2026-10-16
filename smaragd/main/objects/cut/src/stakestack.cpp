#include "app/objects/cut/stakestack.h"
#include <utility>
#include <vector>
#include "app/model/sautofade.h"
#include "app/model/sclipwindow.h"
#include "app/model/slink.h"
#include "app/model/sproject.h"
#include "app/model/sappcontext.h"
#include "app/persistence/sprojectloader.h"
#include "tw/graph/twcomponent.h"
#include "tw/core/twfraction.h"
#include "tw/events/twtempomap.h"

#include <QDomElement>
#include <QPainter>
#include <algorithm>
#include <cmath>
#include <cstring>

// ---------------------------------------------------------------------------
// Silent placeholder component, served while no take is active. App-side
// twComponent subclassing keeps objects/cut free of a tw/mix dependency
// (twRewire would do the same job but lives outside this slice's engine
// surface — see tools/check_layering.py).
// ---------------------------------------------------------------------------
namespace {
class STakeSilence : public twComponent
{
public:
    explicit STakeSilence( tw303aEnvironment &e ) : twComponent( e ) {}
    bool isSeekable() const override { return true; }
    int seekTo( offset_t ) override { return 0; }
    void reset() override {}
    length_t renderFrames( sample_t *out, length_t n, const sample_t *,
                           length_t, idx_t ) override {
        ::memset( out, 0, n * sizeof( sample_t ) );
        return n;
    }
    void createOutputLatches() override {}
    idx_t getNInputs() const override { return 0; }
    idx_t getNOutputs() const override { return 1; }
    const char *getInputName( idx_t ) const override { return nullptr; }
    const char *getOutputName( idx_t ) const override { return "silence"; }
};
}  // namespace

// ---------------------------------------------------------------------------
// STakeStack
// ---------------------------------------------------------------------------

STakeStack::STakeStack( SProject *project )
    : SObject( project )
{
}

STakeStack::~STakeStack()
{
    delete inlineRenderer_;
    cpSilence_.reset();
}

SObject *STakeStack::takeObjectAt( int index ) const
{
    if( index < 0 || index >= childCount() ) return nullptr;
    SLink *lk = childAt( index );
    return lk ? &lk->getSObject() : nullptr;
}

SClipWindow *STakeStack::takeAt( int index ) const
{
    return SClipWindow::of( takeObjectAt( index ) );
}

SContentKind STakeStack::contentKind() const
{
    // Homogeneous by construction (insertTake refuses a mismatch), so the
    // first take answers for the column. An empty column cannot occur in a
    // loaded project (invariant 3) but can exist for an instant while one is
    // being built — Audio is the default everywhere else too.
    if( SObject *first = takeObjectAt( 0 ) ) return first->contentKind();
    return SContentKind::Audio;
}

SLink *STakeStack::insertTake( SClipWindow &window, int atIndex,
                               offset_t tStart, length_t tLen )
{
    SObject &obj = window.asObject();
    if( childCount() > 0 && window.contentKind() != contentKind() ) {
        qWarning( "STakeStack: refusing a take of content kind %d into a "
                  "column of kind %d — a take is an ALTERNATIVE for the same "
                  "region, not a different instrument.",
                  (int) window.contentKind(), (int) contentKind() );
        return nullptr;
    }
    if( tStart < 0 ) tStart = 0;
    SLink *lk = new SLink( obj, nullptr );
    // THE TAKE'S EXTENT START (invariant 1′). It was unconditionally 0 before
    // plan 53 §4.1, and the default still is — which is what keeps every
    // caller written before extents correct without a change.
    lk->setStartTime( tStart );
    lk->setParent( this );                     // appends to childOrder_
    if( atIndex >= 0 && atIndex < childCount() - 1 ) {
        moveChildToIndex( childCount() - 1, atIndex );
        if( atIndex <= activeTake_ )
            ++activeTake_;                     // keep pointing at the same take
    }
    // tLen < 0 MEANS "the window's own duration", which is the duration every
    // pre-extent caller had already sized its window to; an explicit length
    // (add-take length=) narrows the window to it instead.
    if( tLen >= 0 ) window.setDurationFromTimeline( tLen );
    const length_t len = ( tLen >= 0 ) ? tLen : window.duration();
    // THE COLUMN GROWS TO COVER THE NEW TAKE, never shrinks. That is what
    // makes the FIRST take of a fresh column establish its length, exactly as
    // take 0 used to answer for it before duration_ existed.
    if( tStart + (offset_t) len > (offset_t) duration_ )
        duration_ = (length_t) ( tStart + (offset_t) len );
    QObject::connect( &obj, SIGNAL( durationChanged( length_t ) ),
                      this, SLOT( onTakeCutChanged( length_t ) ) );
    refreshPartial_();
    return lk;
}

// --- take extents (invariant 1′, plan 53 §4.1) ------------------------------

offset_t STakeStack::takeExtentStart( int index ) const
{
    SLink *lk = childAt( index );
    return lk ? lk->getStartTime() : 0;
}

length_t STakeStack::takeExtentLength( int index ) const
{
    SClipWindow *w = takeAt( index );
    return w ? w->duration() : 0;
}

bool STakeStack::takeIsFullExtent( int index ) const
{
    if( index < 0 || index >= childCount() ) return false;
    return takeExtentStart( index ) == 0
        && takeExtentLength( index ) == duration_;
}

void STakeStack::setTakeExtent( int index, offset_t tStart, length_t len )
{
    SLink *lk = childAt( index );
    SClipWindow *w = takeAt( index );
    if( !lk || !w ) return;
    if( tStart < 0 ) tStart = 0;
    if( len < 0 ) len = w->durationBlocking();
    if( len < 1 ) len = 1;
    lk->setStartTime( tStart );
    w->setDurationFromTimeline( len );
    if( tStart + (offset_t) len > (offset_t) duration_ )
        duration_ = (length_t) ( tStart + (offset_t) len );
    refreshPartial_();
}

offset_t STakeStack::nextCompEdgeAfter( offset_t columnPos ) const
{
    offset_t edge = -1;
    const auto nearer = [&edge, columnPos]( offset_t cand ) {
        if( cand <= columnPos ) return;
        if( edge < 0 || cand < edge ) edge = cand;
    };

    // The map's boundaries. Segments are sorted, so the first one past
    // `columnPos` is the nearest and no later one can beat it.
    for( const twCompSegment &cs : compMap_.segments() )
        if( (offset_t) cs.at > columnPos ) { nearer( (offset_t) cs.at ); break; }

    // ...and the chosen take's own extent edges. `takeIndexAt` gives -1 where
    // the chosen take has no material, and the run of -1 ends where that
    // take's material BEGINS — so the edge to find is the extent's start when
    // we are before it and its end when we are inside it, which is exactly
    // `twCompColumn::nextTakeEdgeAfter_nolock`.
    const int t = compMap_.takeAt( (int64_t) columnPos );
    const int chosen = ( t >= 0 && t < childCount() ) ? t : activeTake_;
    if( chosen >= 0 && chosen < childCount() ) {
        const offset_t exStart = takeExtentStart( chosen );
        const offset_t exEnd   = takeExtentEnd( chosen );
        if( columnPos < exStart )     nearer( exStart );
        else if( columnPos < exEnd )  nearer( exEnd );
    }

    if( edge >= (offset_t) duration_ ) return -1;   // holds to the column's end
    return edge;
}

length_t STakeStack::derivedDuration() const
{
    length_t d = 0;
    for( int i = 0; i < childCount(); ++i ) {
        const offset_t e = takeExtentEnd( i );
        if( e > (offset_t) d ) d = (length_t) e;
    }
    return d;
}

void STakeStack::setColumnDuration( length_t duration )
{
    if( duration < 0 ) duration = 0;
    duration_ = duration;
    refreshPartial_();
}

void STakeStack::refreshPartial_()
{
    bool partial = false;
    for( int i = 0; i < childCount(); ++i ) {
        if( !takeIsFullExtent( i ) ) { partial = true; break; }
    }
    hasPartial_ = partial;
}

void STakeStack::removeTake( int index )
{
    SLink *lk = childAt( index );
    if( !lk ) return;
    if( SObject *take = takeObjectAt( index ) ) {
        QObject::disconnect( take, SIGNAL( durationChanged( length_t ) ),
                             this, SLOT( onTakeCutChanged( length_t ) ) );
    }
    delete lk;                                 // refcount release → deleteLater
    if( activeTake_ == index ) {
        activeTake_ = -1;
        emit activeTakeChanged( activeTake_ );
    } else if( activeTake_ > index ) {
        --activeTake_;
    }
    // THE COMP MAP NEEDS THE SAME RENUMBERING (plan 53 B10), and it had none:
    // removing take 0 of a comped column left every segment pointing one take
    // off, and a segment naming the removed take pointed at an index that no
    // longer exists.
    //
    // Mirroring the activeTake_ arithmetic directly above, segment by segment:
    // a segment naming the removed take is DROPPED -- the previous segment
    // then extends over its range, which is the only sensible answer once the
    // material it selected is gone -- and a segment above it is decremented.
    //
    // normalized() (inside setCompMap, and applied here through the same
    // constructor) merges the adjacent same-take segments a drop can create,
    // and re-zeroes a first segment's meaningless xfade. Dropping the FIRST
    // segment leaves the map starting later, so positions before the new first
    // one fall back to activeTake_ -- which has just been renumbered too.
    if( !compMap_.empty() ) {
        std::vector<twCompSegment> kept;
        kept.reserve( compMap_.segments().size() );
        for( const twCompSegment &sg : compMap_.segments() ) {
            if( sg.take == index ) continue;
            twCompSegment out = sg;
            if( out.take > index ) --out.take;
            kept.push_back( out );
        }
        // Assigned directly rather than through setCompMap(), which would emit
        // its own durationChanged: the emit at the end of this function is the
        // one that publishes this change, so the track resyncs once.
        compMap_ = twCompMap( std::move( kept ) ).normalized();
        syncCompTicks_();
    }
    // THE COLUMN KEEPS ITS LENGTH (invariant 1′). Removing a take does not
    // shrink the column: duration_ is the column's own property now, and a
    // span whose only take just went away is a span that plays SILENCE — not
    // a reason to move the clip's right edge under the user.
    refreshPartial_();
    // Track resync (updateClip resets the clip state chain; the render path
    // to the root is re-frozen, siblings keep their caches). Blocking read:
    // never the stale try-lock duration (proposal 19 Phase 2b).
    emit durationChanged( getDurationBlocking() );
}

void STakeStack::setActiveTake( int index )
{
    if( index < -1 || index >= childCount() ) return;
    if( index == activeTake_ ) return;
    activeTake_ = index;                       // the mutation …
    emit activeTakeChanged( index );
    // … then the notification (proposal 15 ordering): the track's slot runs
    // updateClip (content-epoch bump + state-chain reset — our component
    // identity just changed) and invalidateRenderPath. During playback,
    // proposal 16 serves the old take until the new pages land.
    // Blocking read: never the stale try-lock duration (proposal 19 Phase 2b).
    emit durationChanged( getDurationBlocking() );
}

void STakeStack::setCompMap( const twCompMap &map )
{
    const twCompMap next = map.normalized();
    if( next == compMap_ ) return;
    compMap_ = next;                           // the mutation ...
    syncCompTicks_();
    // ... then the notification, in the SAME order and by the SAME route
    // `setActiveTake` uses (proposal 15 ordering): the track's slot runs
    // updateClip -- a content-epoch bump and a state-chain reset, because our
    // component identity just changed -- and invalidateRenderPath. N1 has no
    // consumer, so this publishes a change nothing hears yet; wiring it now is
    // what means N2 adds a component and no plumbing.
    emit durationChanged( getDurationBlocking() );
}

// --- the comp map's musical anchor (plan 53 P3) -----------------------------

void STakeStack::syncCompTicks_()
{
    compTicks_.clear();
    if( contentKind() != SContentKind::Event || compMap_.empty() ) return;
    const twTempoMap &tmap = getProject().tempoMap();
    const int         rate = getProject().getSRate();
    compTicks_.reserve( compMap_.segments().size() );
    for( const twCompSegment &sg : compMap_.segments() )
        compTicks_.push_back(
            tmap.framesToTickLen( (int64_t) sg.at, rate ).ticks() );
}

bool STakeStack::rederiveMusicalPositions()
{
    // An AUDIO column's comp boundary is a position in a recording and must
    // not move when the tempo does -- the same asymmetry `onTakeCutChanged`
    // explains for `duration_`. So: event columns only, and only when
    // `syncCompTicks_` had something to record.
    if( contentKind() != SContentKind::Event ) return false;

    // THE COLUMN'S LENGTH, AGAIN, AND THE ORDER IS THE WHOLE REASON.
    // `onTakeCutChanged` already follows `derivedDuration()` for an event
    // column, but it only fires on a TAKE's duration change -- and a tempo
    // edit moves each take's extent in two independent steps, the LINK's start
    // (`rederiveStartTime`) and the WINDOW's length. Whichever runs second is
    // not announced, so `duration_` could be left mid-move: measured on an
    // undo of `set-tempo` over a two-bar column, `extents=[0-192000;
    // 96000-192000] duration=288000`, which is 192000 + 96000 -- the OLD
    // length added to the NEW start, and a column longer than its takes reach
    // with no reason to be.
    //
    // This is called from the same loop, AFTER every one of this object's
    // links has been re-derived, so here the move is complete.
    bool moved = false;
    {
        const length_t derived = derivedDuration();
        if( derived > 0 && derived != duration_ ) {
            duration_ = derived;
            refreshPartial_();
            moved = true;
        }
    }

    if( compMap_.empty() || compTicks_.size() != compMap_.segments().size() )
        return moved;

    const twTempoMap &tmap = getProject().tempoMap();
    const int         rate = getProject().getSRate();
    std::vector<twCompSegment> segs = compMap_.segments();
    bool mapMoved = false;
    for( std::size_t i = 0; i < segs.size(); ++i ) {
        const int64_t at =
            tmap.ticksToFrames( TickLen( compTicks_[i] ), rate ).floorToInt();
        if( at != segs[i].at ) { segs[i].at = at; mapMoved = true; }
        // `xfade` IS LEFT ALONE, deliberately. It is a crossfade LENGTH in
        // frames, and on an event column it means nothing to begin with:
        // notes do not crossfade, so `twCompColumn`'s equal-power ramp has no
        // event-path counterpart and no gate sets one here. Moving a number
        // nothing reads would be invention, not parity.
    }
    if( !mapMoved ) return moved;
    // Through `setCompMap`: it normalizes and it publishes the change the
    // track needs in order to re-collect the clip. It also re-syncs the ticks
    // from these frames, which would feed the conversion's own rounding back
    // into the anchor, so the anchors are put back verbatim afterwards -- they
    // are what produced these frames and they have not changed.
    const std::vector<Fraction> anchors = compTicks_;
    setCompMap( twCompMap( std::move( segs ) ) );
    compTicks_ = anchors;
    return true;
}

void STakeStack::applyColumnLength_( length_t duration, bool wholeWindow,
                                     length_t loopLength,
                                     const Fraction &stretch )
{
    if( duration < 1 ) duration = 1;
    const length_t oldDur = duration_;
    duration_ = duration;

    forwardSuppressed_ = true;
    for( int i = 0; i < childCount(); ++i ) {
        SClipWindow *take = takeAt( i );
        SLink *lk = childAt( i );
        if( !take || !lk ) continue;

        offset_t tStart = lk->getStartTime();
        length_t len    = take->duration();
        // THE CLAMP (invariant 1′). A take that reached the OLD end was clamped
        // BY the column and follows it; a take that ended earlier was not, and
        // must stay where the user put it. For a full-extent column every take
        // is in the first case, so this is exactly "set every take's duration".
        const bool reachedOldEnd = ( tStart + (offset_t) len == (offset_t) oldDur );
        if( reachedOldEnd ) len = (length_t) ( (offset_t) duration - tStart );
        else if( tStart + (offset_t) len > (offset_t) duration )
            len = (length_t) ( (offset_t) duration - tStart );

        // A take pushed entirely outside the new column. Not refused HERE —
        // refusing needs a verb that can reject, which is `resize-clip`'s job
        // (plan 53 A0b) — so the model clamps to one frame and says so, which
        // keeps invariant 1′'s "⊆ [0, duration_)" true whatever a caller does.
        if( tStart >= (offset_t) duration ) {
            tStart = (offset_t) duration - 1;
            len = 1;
            qWarning( "STakeStack: take %d clamped to one frame — the column's "
                      "new length %lld leaves it no room. resize-clip refuses "
                      "this (plan 53 A0b); a direct model write cannot.",
                      i, (long long) duration );
        }
        if( len < 1 ) {
            len = 1;
            qWarning( "STakeStack: take %d clamped to one frame at %lld — the "
                      "column's new length %lld would have emptied it.",
                      i, (long long) tStart, (long long) duration );
        }

        lk->setStartTime( tStart );
        if( wholeWindow ) {
            // Slip offsets live in the stretched OUTPUT domain: a stretch
            // change rescales them so every take keeps pointing at the same
            // material. The content anchor is authoritative (proposal 18
            // Phase 3): a stretch change does not move it, so the old
            // warped-offset rescale (and its per-take rounding) is gone.
            take->setWindowExact( take->contentAnchorExact(), len,
                                  loopLength, stretch );
        } else {
            take->setDurationFromTimeline( len );
        }
    }
    forwardSuppressed_ = false;
    refreshPartial_();
    // Emit the value we just set on the COLUMN — NOT getDuration(), which used
    // to read the active take via SCut::getSnapshot()'s try-lock and could
    // return the stale pre-edit duration when a background revalidation worker
    // held the cut mutex. That stale value reached the track
    // (STrack::trackChildDurationChanged → twTrackMix::updateClip), leaving a
    // split clip's HEAD at its full pre-split length so it bled into the tail
    // region — the confirmed takes_group_broadcast flake (proposal 19
    // Phase 2b). Emitting the authoritative value is race-free, and since
    // invariant 1′ that value is duration_ rather than any take's.
    emit durationChanged( duration_ );
}

void STakeStack::setDurationAll( length_t duration )
{
    applyColumnLength_( duration, false, 0, Fraction( 1 ) );
}

void STakeStack::setDuration( length_t duration )
{
    setDurationAll( duration );
}

void STakeStack::applyWindowAll( length_t duration, length_t loopLength,
                                 const Fraction &stretch )
{
    applyColumnLength_( duration, true, loopLength, stretch );
}

void STakeStack::onTakeCutChanged( length_t )
{
    if( forwardSuppressed_ ) return;
    // FORWARD FOR EVERY TAKE, not only the audible one (plan 53 §4.1).
    //
    // It used to forward for the active take alone, on the reasoning that only
    // the active take reaches the mix. Under a comp map that was already false
    // (pre-existing since proposal 43 N2, for hand-made comps), and under
    // invariant 1′ it is false for every recording over existing material: an
    // INACTIVE take is audible wherever the map or the extents put it, so a
    // later slip, volume or fade edit on it would bump no epoch and invalidate
    // no range — the edit would simply not be heard until something else
    // happened to re-freeze the page.
    //
    // The cost is a resync the mix already debounces, which is why this is the
    // simplest correct answer rather than a per-take audibility test that
    // would have to re-derive what the map says.
    SObject *obj = dynamic_cast<SObject *>( sender() );
    if( !obj ) return;

    // AN EVENT COLUMN'S OWN LENGTH FOLLOWS ITS TAKES (plan 53 P3, QBX-181).
    //
    // `duration_` is a stored FRAME value and nothing re-derived it, which is
    // a defect the moment the takes are MUSICAL. An event take's window
    // derives its length from `lengthTicks_` and its link is born
    // `timebase=beats`, so `set-tempo` moves both ends of every extent — and
    // left the column at the length it happened to have when it was built.
    // Measured on `main`, one union column over one bar after
    // `set-tempo bpm="60"`: `extents=[0-192000;0-192000] duration=96000`. The
    // takes reach 192000, the column says 96000, and invariant 1′ ("every take
    // has an extent ⊆ [0, duration_)") is false on every tempo change over an
    // event column. What the user sees is the clip's tail cut off.
    //
    // ONLY FOR EVENT CONTENT, and the asymmetry is the design rather than
    // caution. A0a deliberately lets an AUDIO column be longer than its takes
    // reach — a span no take covers plays silence, which is what
    // `merge-columns` relies on for a gap between operands — and an audio
    // take's length changes only through the edit paths that already clamp it
    // (`setDurationAll` / `applyWindowAll` / `setTakeExtent`), all of which
    // keep `duration_` right themselves. Nothing re-derives an EVENT take's
    // length; the tempo map does, behind everyone's back.
    //
    // `derivedDuration()` already exists and is exactly max(tStart + len), so
    // this is one assignment, before `refreshPartial_()` reads `duration_`
    // through `takeIsFullExtent`.
    if( contentKind() == SContentKind::Event ) {
        const length_t derived = derivedDuration();
        if( derived > 0 ) duration_ = derived;
    }

    // A take's own duration IS its extent length, so this is also where a
    // column learns it has become (or stopped being) partial.
    refreshPartial_();
    // Blocking read: never the stale try-lock duration (proposal 19 Phase 2b).
    emit durationChanged( getDurationBlocking() );
}

// --- SObject delegation ----------------------------------------------------

std::shared_ptr<twComponent> STakeStack::ensureSilence()
{
    if( !cpSilence_ ) {
        cpSilence_ = std::make_shared<STakeSilence>(
            *( SAppContext::get().get303aEnvironment() ) );
        cpSilence_->init();
    }
    return cpSilence_;
}

std::shared_ptr<twCompColumn> STakeStack::ensureCompColumn()
{
    if( !cpComp_ ) {
        cpComp_ = std::make_shared<twCompColumn>(
            *( SAppContext::get().get303aEnvironment() ),
            (idx_t) getProject().channels() );
        cpComp_->init();
    }
    // Rebuilt from the CURRENT takes and the CURRENT map on every ask: the
    // column is only reached through resolveClip/getRootComponent, both of
    // which run on the main thread, and a take list that went stale would
    // render a take that no longer exists.
    std::vector<twCompColumn::ResolveFn> fns;
    fns.reserve( (size_t) nTakes() );
    for( int i = 0; i < nTakes(); ++i ) {
        SObject *t = takeObjectAt( i );
        fns.push_back( [t]( offset_t p ) {
            return t ? t->resolveClip( p ) : twResolvedClip{};
        } );
    }
    cpComp_->setTakes( std::move( fns ) );
    // THE EXTENTS, after setTakes (which rebuilds the entry list) and before
    // the map: the column is the ONE object that honours them (invariant 1′),
    // and it needs them in place before anything asks it to plan a page.
    for( int i = 0; i < nTakes(); ++i )
        cpComp_->setInputExtent( i, takeExtentStart( i ),
                                 takeExtentLength( i ) );
    // THE PROJECT'S AUTO-FADE (plan 53 P5, D3a): the length a boundary with no
    // crossfade of its own gets. Set before the map, with the extents, because
    // the column clamps it against both — and set on EVERY ask, like the takes
    // and the map, so a change to the project property is picked up by the next
    // resolve rather than needing the column torn down.
    //
    // A column that resolves through this path always HAS boundaries: with no
    // map and every take full, `getRootComponent` hands out the active take's
    // own component and never builds a column at all (the degenerate case,
    // proposal 43 N2). So the auto-fade reaches exactly the joins that exist,
    // and a project written before plan 53 — whose property reads 0 — renders
    // through here byte for byte as it did.
    cpComp_->setAutoXfade( (int64_t) sautofade::framesOf( getProjectSafe() ) );
    // EACH TAKE'S OWN GAIN (QBX-185), after setTakes for the same reason the
    // extents are and on every ask for the same reason the auto-fade is: a
    // `set-clip-volume take=` edit is picked up by the next resolve rather
    // than needing the column torn down.
    //
    // WHY IT HAS TO BE HERE AT ALL. These four values used to reach the mix
    // only through `STrack::refreshClipGainCurves`, which composes ONE entry
    // per placement and reads it from `SClipWindow::parametersOf` — the ACTIVE
    // take. The column renders each region from whichever take the map
    // selects, so an inactive take's own volume, pan, fade and envelope were
    // carried, serialized, editable and never heard. The funnel now pushes
    // NEUTRAL for a column that resolves through here
    // (`appliesOwnWindowGain`), and the per-region truth is these calls.
    for( int i = 0; i < nTakes(); ++i ) {
        twCompColumn::TakeGain g;
        // THE TAKE'S OWN WINDOW, not `parametersOf` and not the active take:
        // one take, one set of parameters, which is the whole point.
        if( SClipWindow *w = takeAt( i ) ) {
            SObject &o = w->asObject();
            // dB to linear, so it composes with the envelope as a PRODUCT,
            // i.e. the two sum in dB — the identical conversion and the
            // identical reason as `setClipGainScalar`'s call site
            // (mix/CONTRACT.md inv. 21).
            g.scalar = std::pow( 10.0, o.getVolume() / 20.0 );
            g.pan    = o.getPan();
            g.curve  = o.automationCurve( QStringLiteral( "cut:Gain" ) );
            // A FADE IS AUDIO-ONLY (cut/CONTRACT inv. 4), and an event take's
            // edges are note-offs rather than steps in a waveform — the same
            // guard, and the same reason, as the auto-fade's in
            // `refreshClipGainCurves`.
            if( o.contentKind() == SContentKind::Audio ) {
                g.fade = o.clipFade();
                // THE TAKE'S OWN LENGTH, because a fade-out is anchored to
                // the end of the window it belongs to. Not the extent length
                // in the column's frames: the two coincide for a bounded take
                // (invariant 1') and differ for an unbounded one.
                g.fadeLen = o.getDurationBlocking();
            }
        }
        cpComp_->setInputGain( i, g );
    }
    cpComp_->setCompMap( compMap_, activeTake_ );
    return cpComp_;
}

std::shared_ptr<twComponent> STakeStack::getRootComponent()
{
    // THE RESOLUTION RULE (plan 53 §4.1): a map OR a partial take goes through
    // the comp column. No map and every take full is the DEGENERATE case and
    // takes the ORIGINAL path, byte for byte (proposal 43 N2).
    if( resolvesThroughComp() ) return ensureCompColumn();
    if( SObject *take = activeTakeObject() )
        return take->getRootComponent();
    return ensureSilence();
}


QList<SObject::SDirtyRange> STakeStack::mapChildRangesToSelf(
    SLink *childLink, const QList<SDirtyRange> &childRanges )
{
    // EVERY TAKE MAPS (plan 53 §4.1), for the reason onTakeCutChanged spells:
    // a comped or partial column makes an inactive take audible over its own
    // span, so an edit inside it must reach an epoch. The old body returned {}
    // for every take but the active one, which is what made a slip on a comped
    // inactive take inaudible until something else re-froze the page.
    if( !childLink ) return {};
    const int index = indexOfChild( childLink );
    if( index < 0 ) return {};

    // The base already SHIFTS by the child link's startTime -- and since
    // invariant 1' that start time IS the take's tStart, so the shift the
    // containment map has always applied is exactly the one extents need.
    const QList<SDirtyRange> shifted =
        SObject::mapChildRangesToSelf( childLink, childRanges );

    // ...then INTERSECT with the extent: an edit outside the take's own span
    // is windowed away and must bump nothing, which is what keeps a partial
    // take's invalidation as narrow as a plain clip's.
    const offset_t s = takeExtentStart( index );
    const offset_t e = takeExtentEnd( index );
    QList<SDirtyRange> out;
    for( const SDirtyRange &r : shifted ) {
        const offset_t a = std::max( r.start, s );
        const offset_t b = std::min( r.end, e );
        if( b > a ) out.append( { a, b } );
    }
    return out;
}

bool STakeStack::windowStep( offset_t clipRel, SWindowStep &out ) const
{
    SObject *take = activeTakeObject();
    if( !take ) return false;               // no active take: nothing audible
    // SUBTRACT tStart AND REFUSE OUTSIDE THE EXTENT (invariant 1′). The
    // identity this used to be is the full-extent case of exactly this, and a
    // position the active take has no material for is not a position IN it —
    // reporting it would make a position walk claim an arrangement that is
    // silent there.
    const int index = activeTake_;
    const offset_t s = takeExtentStart( index );
    const offset_t e = takeExtentEnd( index );
    if( clipRel < s || clipRel >= e ) return false;
    out.content = take;
    out.pos     = clipRel - s;
    return true;
}

twEventClipResolved STakeStack::resolveEventClip( offset_t clipPos )
{
    // SHIFTED BY THE ACTIVE TAKE'S EXTENT (plan 53 §4.1's P3 row, QBX-181).
    //
    // The track inserts a column as ONE clip spanning the column, and hands
    // this `clipPos` in the COLUMN's frames. The take answers in its OWN
    // frames, where zero is where ITS window starts -- which under invariant
    // 1' is the extent start, not the column's origin. Unshifted, a recording
    // over bars 8-16 of a 32-bar part sounded at bar 0: measured before this,
    // the feed over the union column gave the pass's notes at frames 0 and
    // 24000 where they were played at 768000 and 792000.
    //
    // The whole fix is composing the two maps, which is the event twin of
    // `mapTimelineToComponentPos` on the audio side.
    const int active = activeTake_;
    SObject *take = takeObjectAt( active );
    if( !take ) return twEventClipResolved{};
    const offset_t tStart = takeExtentStart( active );

    twEventClipResolved out =
        take->resolveEventClip( clipPos - tStart );
    if( tStart == 0 ) return out;          // nothing to compose

    // COMPOSE, never replace: the take's own map carries its slip and its loop
    // (`SMidiCut::resolveEventClip` picks `twEventSlipMap` or
    // `twEventLoopMap`), and a column position has to go through BOTH. A
    // single slip map built here would silently drop a looped take's wraps --
    // and with them `runFrames`, which is what tells the enumeration where the
    // mapping breaks.
    const twEventMapPosFn inner = out.map;
    const int64_t shift = (int64_t) tStart;
    out.map = [inner, shift]( int64_t columnPos ) -> twEventMapping {
        const int64_t takePos = columnPos - shift;
        if( !inner ) return twEventMapping{ takePos, INT64_MAX };
        return inner( takePos );
    };
    return out;

    // WHAT THIS STILL DOES NOT DO, and why it is not done here. The COMP MAP
    // is not consulted: wherever the active take has no material the column
    // plays SILENCE rather than the take the map names, so a mode A recording
    // over the middle of a part leaves the part's own bars silent. That needs
    // what the audio side has and the event side does not -- a `twCompColumn`
    // equivalent -- and this seam cannot express it: `resolveEventClip`
    // returns ONE sequence per clip, and two takes are two sequences. The fix
    // is for the column to SYNTHESISE one sequence from its takes (each
    // take's events shifted into column frames and masked by the map and the
    // extents, which an internal `twEventClipSet` can collect), and that is
    // its own change with its own gates. Filed as QBX-189 rather than
    // half-built; plan 53 §4.1 already said "the event path has no
    // twCompColumn to do that in".
}

offset_t STakeStack::mapTimelineToComponentPos( offset_t off )
{
    // THROUGH THE COMP COLUMN (plan 53 §4.1), which means the IDENTITY: the
    // component a comped-or-partial column resolves to is the column itself,
    // addressed in the column's own frame domain. Delegating to the active
    // take would hand back a position in a DIFFERENT component's domain.
    if( resolvesThroughComp() ) return off;
    if( SObject *take = activeTakeObject() )
        return take->mapTimelineToComponentPos( off );
    return off;
}

twResolvedClip STakeStack::resolveClip( offset_t off )
{
    // A comped column resolves to ITSELF, at the column's own position: the
    // map is read per REGION inside freezePage, which is the whole point --
    // the mix resolves once per PAGE and a page is 1.37 s, far coarser than a
    // comp boundary.
    if( resolvesThroughComp() )
        return twResolvedClip{ ensureCompColumn(), off };
    // Read the active take ONCE and resolve the whole clip through it, so the
    // component and the mapping can't come from different takes (getRootComponent
    // and mapTimelineToComponentPos used to resolve the take separately) nor from
    // different reader generations of the same take (SCut::resolveClip fuses it).
    if( SObject *take = activeTakeObject() )
        return take->resolveClip( off );
    return twResolvedClip{ ensureSilence(), off };
}

int STakeStack::seekTo( offset_t off )
{
    // THROUGH THE COMP COLUMN (plan 53 §4.1): it seeks every take it holds,
    // which is what a column whose takes are each only audible somewhere
    // needs. Seeking the active take alone left every other contributing take
    // where the last seek had put it.
    if( resolvesThroughComp() ) return ensureCompColumn()->seekTo( off );
    if( SObject *take = activeTakeObject() )
        return take->seekTo( off );
    return 0;
}

length_t STakeStack::getDuration() const
{
    // THE COLUMN'S OWN LENGTH (invariant 1′). It used to be the ACTIVE take's,
    // else take 0's — which is precisely what a partial take makes untenable:
    // W1's column, whose active take covers only its first half, would report
    // half its length and the clip's right edge would move as the user clicked
    // between takes.
    //
    // NO LOCK AND NO STALE-VALUE HAZARD, which is the quiet win here: the old
    // body went through SCut::getSnapshot()'s try-lock and could return the
    // pre-edit duration under background-worker contention (proposal 19
    // Phase 2b), which is why getDurationBlocking() had to exist at all for a
    // column. duration_ is the column's own field, written on the main thread.
    return duration_;
}

length_t STakeStack::getDurationBlocking() const
{
    // Identical to getDuration() since invariant 1′ — see there. Kept as its
    // own override because the SObject seam names it and every caller on the
    // edit/signal path asks for it by this name.
    return duration_;
}

bool STakeStack::hasPreview() const
{
    // NO EXTENT TEST HERE, deliberately. An extent is at least one frame
    // (`setTakeExtent` and the write-through both clamp to 1), so a take that
    // exists always has SOME material and the honest answer to "is there a
    // preview" is still its own. What 1' changes is WHERE that material is,
    // which is getPreview's business below.
    SObject *take = activeTakeObject();
    return take ? take->hasPreview() : false;
}

int STakeStack::getPreview( preview_t *dest, offset_t start, length_t length,
                            offset_t nProbes )
{
    SObject *take = activeTakeObject();
    if( !take ) return -1;
    if( !dest || nProbes < 1 || length < 1 ) return -1;

    // THE DEGENERATE COLUMN TAKES THE ORIGINAL PATH, byte for byte: the active
    // take covers [0, duration_), so every probe is its own and there is
    // nothing to window. Same rule as the audio resolution (see
    // resolvesThroughComp) and the painters below, for the same reason.
    if( takeIsFullExtent( activeTake_ ) )
        return take->getPreview( dest, start, length, nProbes );

    // WINDOWED TO THE ACTIVE TAKE'S EXTENT (plan 53 §4.1, handed over by A0a).
    // Outside it the column has NO MATERIAL, and a preview that answered the
    // take's samples there would claim a waveform where silence plays. Zero is
    // how this seam spells "no material" — `fillBodyByMaterial` and the
    // folder-sum overlay already read `min == 0 && max == 0` as a gap.
    for( offset_t i = 0; i < nProbes; ++i ) { dest[ i ].min = 0; dest[ i ].max = 0; }

    const offset_t exStart = takeExtentStart( activeTake_ );
    const offset_t exEnd   = takeExtentEnd( activeTake_ );

    // Probe i spans column positions starting at `start + (i*length)/nProbes`.
    // Inverting that grid with CEILING division keeps a probe out of the range
    // unless its own start is inside the extent, so no probe is ever filled
    // from material the take does not have there. A probe STRADDLING an extent
    // edge is resolved by its start, which is the same half-open convention
    // `takeCoversPos` uses; at preview resolution that is at most one column.
    const auto probeStart = [start, length, nProbes]( offset_t i ) -> offset_t {
        return start + (offset_t) ( ( (int64_t) i * (int64_t) length )
                                    / (int64_t) nProbes );
    };
    const auto ceilProbe = [length, nProbes]( int64_t d ) -> offset_t {
        if( d <= 0 ) return 0;
        if( d >= (int64_t) length ) return (offset_t) nProbes;
        return (offset_t) ( ( d * (int64_t) nProbes + (int64_t) length - 1 )
                            / (int64_t) length );
    };

    offset_t i0 = ceilProbe( (int64_t) ( exStart - start ) );
    offset_t i1 = ceilProbe( (int64_t) ( exEnd   - start ) );
    if( i0 < 0 ) i0 = 0;
    if( i1 > nProbes ) i1 = nProbes;
    if( i1 <= i0 ) return 0;        // the window misses the extent entirely

    const offset_t subStart = probeStart( i0 ) - exStart;   // TAKE-relative
    const length_t subLen   = (length_t) ( probeStart( i1 ) - probeStart( i0 ) );
    if( subLen < 1 ) return 0;
    const int rc = take->getPreview( dest + i0, subStart, subLen, i1 - i0 );
    // A failing sub-collect leaves the zeros, which is the right answer for
    // "no material known" on this seam and keeps the caller's array valid.
    return rc < 0 ? rc : 0;
}

QWidget *STakeStack::getDetailEditWidget( QWidget * )
{
    return nullptr;
}

QWidget *STakeStack::getInlineEditWidget( QWidget * )
{
    return nullptr;
}

SObjectRenderer *STakeStack::getInlineRenderer()
{
    if( !inlineRenderer_ )
        inlineRenderer_ = new STakeStackRendererInline( *this );
    return inlineRenderer_;
}

// --- serialization -----------------------------------------------------------

int STakeStack::serializeSelfAttributes( QTextStream &o )
{
    SObject::serializeSelfAttributes( o );
    o << " activeTake=\"" << activeTake_ << "\"";
    // THE COLUMN'S LENGTH, WRITTEN ONLY WHEN IT CARRIES INFORMATION (plan 53
    // §4.1): a column whose length equals max_i( tStart_i + len_i ) is fully
    // described by its takes, and the loader derives exactly that maximum when
    // the attribute is absent. So a full-extent column — every file written
    // before FORMAT_VERSION 3, and every golden — re-serializes with NO
    // duration= attribute at all and byte-identically.
    //
    // It becomes informative when the column is LONGER than its takes reach:
    // Q5's one-take placeholder (§4.2) and any column whose last take was
    // removed are the cases, and both are unrepresentable without it.
    if( duration_ != derivedDuration() )
        o << " duration=\"" << (qlonglong) duration_ << "\"";
    return 0;
}

int STakeStack::serializeInlineChildren( QTextStream &o )
{
    // Written only when non-empty, exactly like `<automation>`: an empty map
    // is the degenerate "activeTake_ everywhere", so every file written before
    // proposal 43 -- and every golden -- re-serializes byte-unchanged.
    if( compMap_.empty() ) return 0;
    o << "<comp>\n";
    for( const twCompSegment &s : compMap_.segments() ) {
        o << "<seg at='" << (qlonglong) s.at << "' take='" << s.take << "'";
        if( s.xfade != 0 ) o << " xfade='" << (qlonglong) s.xfade << "'";
        o << "/>\n";
    }
    o << "</comp>\n";
    return 0;
}

int STakeStack::readPostChildrenAttributes( QDomElement &element )
{
    // The base call is not decoration: since proposal 37 P5 it is what reads
    // the inline <automation> child. A stack carries no lane of its own today
    // (a clip envelope lives on each TAKE), but an override that silently
    // dropped a known payload is exactly the trap the next owner would fall in.
    SObject::readPostChildrenAttributes( element );

    const QDomElement compEl = element.firstChildElement( "comp" );
    if( !compEl.isNull() ) {
        std::vector<twCompSegment> segs;
        for( QDomElement e = compEl.firstChildElement( "seg" ); !e.isNull();
             e = e.nextSiblingElement( "seg" ) ) {
            twCompSegment s;
            s.at    = e.attribute( "at", "0" ).toLongLong();
            s.take  = e.attribute( "take", "0" ).toInt();
            s.xfade = e.attribute( "xfade", "0" ).toLongLong();
            segs.push_back( s );
        }
        compMap_ = twCompMap( std::move( segs ) ).normalized();
        syncCompTicks_();
    }

    int at = element.attribute( "activeTake", "-1" ).toInt();
    if( at < -1 || at >= childCount() ) at = -1;
    activeTake_ = at;

    // THE COLUMN'S LENGTH (plan 53 §4.1). Absent means "as the takes imply
    // it", which is every file written before FORMAT_VERSION 3 — their takes
    // are all full-extent and all the same length, so the maximum IS that
    // length and nothing about them loads differently.
    //
    // AFTER the takes, necessarily: instantiateFromDomElement has already
    // inserted every one of them (that is what readPostChildrenAttributes
    // means), so derivedDuration() has something to derive from.
    if( element.hasAttribute( "duration" ) )
        duration_ = (length_t) element.attribute( "duration", "0" ).toLongLong();
    else
        duration_ = derivedDuration();
    refreshPartial_();
    return 0;
}

SLink *STakeStack::instantiateFromDomElement(
    SProjectLoader &projectLoader, QDomElement &element, SObject *parent )
{
    (void) parent;
    STakeStack *stack = new STakeStack( &projectLoader.getProject() );
    stack->readPreChildrenAttributes( element );
    QDomNode childNode = element.firstChild();
    while( !childNode.isNull() ) {
        if( childNode.isElement() && childNode.nodeName() == "SLink" ) {
            QDomElement childElement = childNode.toElement();
            QString objectId = childElement.attribute( "objectId" );
            SLink *contentLink =
                projectLoader.getObjectDictionary().value( objectId );
            if( contentLink ) {
                if( SClipWindow *take =
                        SClipWindow::of( &contentLink->getSObject() ) ) {
                    // THE TAKE'S EXTENT START, READ RATHER THAN DISCARDED
                    // (plan 53 §4.1). A take link's startTime has always been
                    // written (SLink::hasStartTime() is unconditionally true),
                    // always as 0, and this loader threw it away — insertTake
                    // forced setStartTime( 0 ). Under invariant 1′ it is the
                    // take's tStart, and the take's own duration (already
                    // loaded onto its window) is the extent's length, so
                    // tLen = -1 reads it from there.
                    //
                    // For every file written before FORMAT_VERSION 3 this
                    // parses the 0 that was always there, which is exactly
                    // what the old forced 0 produced.
                    const offset_t tStart = (offset_t) parseFractionOrDouble(
                        childElement.attribute( "startTime", "0" )
                            .toStdString() ).toDouble();
                    // May be refused on a content-kind mismatch (see
                    // insertTake); that warns for itself and costs one take,
                    // never the column.
                    stack->insertTake( *take, -1, tStart, -1 );
                } else {
                    qWarning( "STakeStack: child %s is not a clip window, "
                              "skipped", qPrintable( objectId ) );
                }
            } else {
                qWarning( "STakeStack: object not in dictionary: %s",
                          qPrintable( objectId ) );
            }
        }
        childNode = childNode.nextSibling();
    }
    stack->readPostChildrenAttributes( element );
    return new SLink( *stack );
}

static const bool s_reg_stakestack = (
    SProjectLoader::registerSObjectClass(
        "STakeStack", STakeStack::instantiateFromDomElement,
        // A CONTAINER of takes: one unloadable take costs its own link, never
        // the column and its siblings (proposal 37 D8a).
        SElementKind::Container ), true );

// ---------------------------------------------------------------------------
// STakeStackRendererInline
// ---------------------------------------------------------------------------

namespace {

/**
 * A render context SHIFTED INTO ONE TAKE'S OWN DOMAIN (plan 53 §4.1 / A0b).
 *
 * A take's material starts at column frame `tStart`, but the take's own
 * renderer indexes from its own zero — every content renderer spells its
 * position as `ctx.getTimeOf( x ) - lk.getStartTime()`, which yields the
 * COLUMN position. Subtracting `tStart` from the time the context reports
 * therefore hands the take exactly its own offset, through the one seam the
 * whole delegation already goes through. For `tStart == 0` it is the identity,
 * which is why a full-extent take is drawn by literally the old code path.
 *
 * THE SHIFT GOES IN THE CONTEXT RATHER THAN IN A TEMPORARY SLink because the
 * link is SHARED: it is the placement every other reader sees, and the take
 * renderers also read its loop, stretch and colours from it. Shifting the
 * clock is local to this draw; re-pointing the link would not be.
 */
class TakeShiftContext : public SRenderContext
{
public:
    TakeShiftContext( SRenderContext &base, offset_t shift )
        : SRenderContext( base.getPainter() ), base_( base ), shift_( shift )
    {
        setVisibRect( base.getVisibRect() );
        setClipColors( base.clipColors() );
    }
    offset_t getTimeOf( int x ) const override
    { return base_.getTimeOf( x ) - shift_; }
private:
    SRenderContext &base_;
    offset_t        shift_;
};

/**
 * ONE RUN of consecutive columns over which the SAME take sounds.
 *
 * `take` is `STakeStack::takeIndexAt`'s answer, so -1 means NO MATERIAL — the
 * hatched span of §4.4, not a take index to look up.
 */
struct CompRun {
    int      c0    = 0;    // first column (pixel or probe) of the run
    int      c1    = 0;    // one PAST the last
    int      take  = -1;
    offset_t tStart = 0;   // that take's extent start; the shift to draw it by
};

/**
 * Group `[0, n)` columns into runs by WHICH TAKE SOUNDS at each one.
 *
 * `columnPosOf( i )` gives column `i`'s position in the STACK's own frame
 * domain. The walk is PER COLUMN and uses only that FORWARD map, which is the
 * whole reason it is shaped this way: the map from a pixel to a column frame
 * is affine for a plain or stretched clip but PIECEWISE for a warped one
 * (`cutSourceTimeOf`'s anchor path) and per-tile for a looped one, so
 * inverting it to find a segment's pixel would be wrong on exactly the clips
 * comping is used on. Asking forward, column by column, cannot be wrong — and
 * it is the same per-column cost `drawGainEnvelope` and `fillBodyByMaterial`
 * already pay.
 *
 * ONE walk for the paint and the collect, so the envelope a script reads and
 * the picture on screen cannot disagree about where a take begins.
 */
template <typename PosFn>
std::vector<CompRun> compRuns( const STakeStack &st, int n, PosFn columnPosOf )
{
    std::vector<CompRun> runs;
    if( n < 1 ) return runs;
    for( int i = 0; i < n; ++i ) {
        const offset_t pos = columnPosOf( i );
        const int t = st.takeIndexAt( (int64_t) pos );
        if( !runs.empty() && runs.back().take == t ) { runs.back().c1 = i + 1; continue; }
        CompRun r;
        r.c0 = i;
        r.c1 = i + 1;
        r.take = t;
        r.tStart = ( t >= 0 ) ? st.takeExtentStart( t ) : 0;
        runs.push_back( r );
    }
    return runs;
}

}  // namespace

STakeStackRendererInline::STakeStackRendererInline( STakeStack &stack )
    : SObjectRenderer( (SObject &)stack )
{
}

STakeStack &STakeStackRendererInline::stack() const
{
    return (STakeStack &)getObject();
}

SObject *STakeStackRendererInline::takeFor( int takeIndex ) const
{
    STakeStack &st = stack();
    return takeIndex < 0 ? st.activeTakeObject() : st.takeObjectAt( takeIndex );
}

void STakeStackRendererInline::drawTake( SLink &lk, SRenderContext &ctx,
                                         int takeIndex )
{
    // The take-lane terminal. NO "no take" hatch and NO n/N badge: a take lane
    // draws one alternative, and a row that this column has no take for is
    // skipped by the caller before it ever gets here (drawTakeLane's
    // "this stack has fewer takes" continue). Same link, same context, so the
    // domain map the caller already folded — a wrapping cut's slip, stretch and
    // loop tiling — is the one this take is drawn in.
    // `takeIndex < 0` MEANS "THE AUDIBLE ONE", AND FOR A COLUMN THE AUDIBLE
    // ONE IS THE COMP RESULT — so it is draw(), exactly as the base class
    // default would have done. Reading it as "the active take, windowed"
    // instead is a mistake that silently reroutes every ORDINARY clip paint
    // through the per-take path: measured here, it made a comped column's
    // envelope come back as the active take's extent rather than the comp,
    // which an all-zero span can hide.
    if( takeIndex < 0 ) { draw( lk, ctx ); return; }

    SObject *take = takeFor( takeIndex );
    if( !take ) return;
    SObjectRenderer *rndr = take->getInlineRenderer();
    if( !rndr ) return;

    STakeStack &st = stack();
    const int t = takeIndex;
    // ...AND ONLY OVER ITS OWN EXTENT, since 1' (plan 53 §4.4: "each take row
    // draws its material over its extent only"). A full-extent take is the
    // original call, unshifted and unclipped.
    if( st.takeIsFullExtent( t ) ) {
        rndr->draw( lk, ctx );              // my link but his object (CLIP_MODEL)
        return;
    }

    QPainter &p = ctx.getPainter();
    const QRect vr = ctx.getVisibRect();
    const offset_t base = lk.getStartTime();
    const offset_t exStart = st.takeExtentStart( t );
    const offset_t exEnd   = st.takeExtentEnd( t );

    // The extent's PIXEL span, found by asking the forward map per column
    // (see compRuns for why it is never inverted). Clipping is what keeps the
    // waveform inside the extent while the take's own renderer still lays its
    // material out across the whole rect — so a take drawn on a take lane and
    // the same take drawn in a comp segment line up to the pixel.
    int x0 = -1, x1 = -1;
    for( int x = vr.left(); x <= vr.right(); ++x ) {
        const offset_t pos = ctx.getTimeOf( x ) - base;
        if( pos >= exStart && pos < exEnd ) { if( x0 < 0 ) x0 = x; x1 = x + 1; }
    }
    if( x0 < 0 ) return;                    // the extent is off screen

    TakeShiftContext shifted( ctx, exStart );
    p.save();
    p.setClipRect( QRect( x0, vr.y(), x1 - x0, vr.height() ), Qt::IntersectClip );
    rndr->draw( lk, shifted );
    p.restore();
}

bool STakeStackRendererInline::collectTakeEnvelope( SLink &lk,
                                                    const SEnvelopeWindow &win,
                                                    preview_t *out,
                                                    int takeIndex )
{
    // See drawTake: -1 is the COMP RESULT, not the active take windowed.
    if( takeIndex < 0 ) return collectEnvelope( lk, win, out );

    SObject *take = takeFor( takeIndex );
    if( !take ) return false;
    SObjectRenderer *rndr = take->getInlineRenderer();
    if( !rndr ) return false;

    STakeStack &st = stack();
    const int t = takeIndex;
    if( st.takeIsFullExtent( t ) )
        return rndr->collectEnvelope( lk, win, out );

    // WINDOWED, like drawTake above. Outside the extent the probes stay ZERO,
    // which is how this seam spells "no material" — `fillBodyByMaterial` leaves
    // such a column unpainted, so the take-lane fill shows through exactly
    // where the take has nothing.
    const int w = win.width < 1 ? 1 : win.width;
    for( int i = 0; i < w; ++i ) { out[ i ].min = 0; out[ i ].max = 0; }

    const offset_t base = lk.getStartTime();
    const offset_t exStart = st.takeExtentStart( t );
    const offset_t exEnd   = st.takeExtentEnd( t );
    // An SEnvelopeWindow is linear BY CONTRACT (sobjectrenderer.h), so the
    // probe grid is interpolated rather than asked per column here.
    const auto posOf = [&win, w, base]( int i ) -> offset_t {
        const int64_t span = (int64_t) win.rightTime - (int64_t) win.leftTime;
        return (offset_t) ( (int64_t) win.leftTime
                            + ( span * (int64_t) i ) / (int64_t) w ) - base;
    };

    int i0 = -1, i1 = -1;
    for( int i = 0; i < w; ++i ) {
        const offset_t pos = posOf( i );
        if( pos >= exStart && pos < exEnd ) { if( i0 < 0 ) i0 = i; i1 = i + 1; }
    }
    if( i0 < 0 ) return true;               // nothing of this take is in view

    SEnvelopeWindow sub;
    sub.leftTime  = posOf( i0 ) + base - exStart;
    sub.rightTime = ( i1 < w ? posOf( i1 ) : (offset_t) ( win.rightTime - base ) )
                    + base - exStart;
    sub.width     = i1 - i0;
    if( sub.width < 1 ) return true;
    // A declining sub-collect leaves the zeros: an event take has no waveform
    // and its extent is still an extent, so TRUE is the honest answer — the
    // caller must not fall back to a solid fill over a windowed take.
    rndr->collectEnvelope( lk, sub, out + i0 );
    return true;
}

// The brush a span with NO MATERIAL is drawn in: the SAME hatch the "no take"
// column has always used. §4.4 says the uncovered span is "hatched, as `no
// take` is drawn today", and one brush is what makes the two read as the same
// statement — "there is nothing here" — rather than as two different ones.
static QBrush noMaterialBrush()
{
    return QBrush( QColor( 70, 70, 70 ), Qt::BDiagPattern );
}

/**
 * THE CUT MARKS (plan 53 §4.4 part 2, QBX-157): "lightly indicating the
 * cutting positions".
 *
 * There are two kinds and they are one kind in the code, which is the point:
 *
 *   INSIDE the column, every boundary of the run walk `draw()` has already
 *   made -- so a comp boundary and the edge of a take's EXTENT are marked by
 *   the same rule, because they are the same thing to a listener (the material
 *   changes here). Walking the runs rather than `compMap().segments()` is what
 *   buys that: `takeIndexAt` folds the extents in, and a mark loop over the
 *   segments alone would miss the edge where a partial take runs out and leave
 *   the one visible discontinuity in the picture unexplained.
 *
 *   AT the column's own two edges, where a split or a record edge cut the
 *   material. This is the case the brief is really about: two columns a
 *   `takes-split` pass left abutting carry the same body colour over
 *   continuous-looking material, so without a mark the cut between them is
 *   invisible.
 *
 * TWO SHORT TICKS, NOT ONE FULL-HEIGHT LINE, and that is load-bearing twice
 * over. On screen a full-height ruling drawn across the waveform competes with
 * the content it annotates, which is the opposite of recessive. And for the
 * gates: the pixel classifiers (`assert-take-lane`, `assert-lane-overlay`)
 * decide "material" per COLUMN by whether the body colour appears anywhere in
 * it, so an opaque full-height mark would turn its own column into a GAP --
 * a cut mark would manufacture the very discontinuity it describes, and
 * `spanFirst` / `gapRuns` would move in every composite-lane case already in
 * the suite. Ticks leave the middle of the column alone, so they are visible
 * to a classifier that LOOKS for them and invisible to every one that does not.
 */
static void drawCutMarks( QPainter &p, const QRect &vr, SRenderContext &ctx,
                          STakeStack &st, SLink &lk,
                          const std::vector<CompRun> &runs )
{
    if( vr.width() < 1 || vr.height() < 4 ) return;

    const int tick = qMax( 2, vr.height() / 6 );
    const int yTop = vr.top();
    const int yBot = vr.bottom() - tick + 1;
    p.setPen( ctx.clipColors().cutMark );
    auto mark = [&]( int x ) {
        if( x < vr.left() || x > vr.right() ) return;
        p.drawLine( x, yTop, x, yTop + tick - 1 );
        p.drawLine( x, yBot, x, yBot + tick - 1 );
    };

    // INSIDE: one mark per run boundary. Run 0 starts at the left edge of the
    // visible rect, which is NOT a boundary of anything -- it is where the
    // viewport happens to begin -- so the walk starts at 1.
    for( size_t i = 1; i < runs.size(); ++i )
        mark( vr.left() + runs[ i ].c0 );

    // THE COLUMN'S OWN EDGES, told apart from a VIEWPORT CLAMP by TIME and not
    // by geometry. The clip loop hands this renderer a rect already cut down to
    // the viewport (`vr.setLeft( visibRect.x() )`), so `vr.left()` is the
    // column's left edge on one repaint and an arbitrary scroll position on the
    // next; marking it unconditionally would draw a cut in the middle of the
    // arrangement as soon as the user scrolled.
    //
    // The comparison is in ONE PIXEL'S WORTH OF TIME rather than in frames: the
    // clip is inset by a pixel (`startX + 1`), and how many frames a pixel is
    // depends on the zoom and, on a warped clip, on where you ask -- so a frame
    // tolerance would be right at exactly one zoom level.
    //
    // It also answers correctly for the WRAPPED shape (`SLink -> SCut ->
    // STakeStack`, a placed column): a slipped wrapper window shows the middle
    // of the material, the column's own frame 0 is nowhere on screen, and no
    // edge mark is drawn -- which is right, because that clip edge is a WINDOW
    // edge and not a cut in the material.
    const offset_t base = lk.getStartTime();
    const offset_t t0   = ctx.getTimeOf( vr.left() );
    const offset_t px   = qAbs( ctx.getTimeOf( vr.left() + 1 ) - t0 );
    const offset_t tol  = 2 * px + 1;
    const offset_t dur  = (offset_t) st.getDuration();
    if( qAbs( t0 - base ) <= tol ) mark( vr.left() );
    if( qAbs( ctx.getTimeOf( vr.right() + 1 ) - base - dur ) <= tol )
        mark( vr.right() );
}

void STakeStackRendererInline::draw( SLink &lk, SRenderContext &ctx )
{
    QPainter &p = ctx.getPainter();
    const QRect vr = ctx.getVisibRect();
    STakeStack &st = stack();

    // HOISTED out of the comp branch below so the cut marks can walk the SAME
    // runs the paint did, rather than making a second walk that could disagree
    // with it about where a take begins. A degenerate column leaves it empty,
    // which is the honest answer: no map and no partial take means no cut
    // INSIDE the column, only its own two edges.
    std::vector<CompRun> runs;

    if( !st.activeTakeObject() && st.nTakes() == 0 ) {
        p.fillRect( vr, noMaterialBrush() );
        p.setPen( QColor( 150, 150, 150 ) );
        p.drawText( vr, Qt::AlignCenter, QStringLiteral( "no take" ) );
    } else if( !st.resolvesThroughComp() ) {
        // THE DEGENERATE COLUMN KEEPS THE ORIGINAL PATH, byte for byte: no
        // map, every take full-extent, so the comp result IS the active take
        // across the whole clip. Same rule as the audio resolution
        // (resolvesThroughComp) and for the same reason — every project that
        // has never been comped must look exactly as it did, which is what
        // the screenshot gates assert.
        if( SObject *take = st.activeTakeObject() )
            if( SObjectRenderer *rndr = take->getInlineRenderer() )
                rndr->draw( lk, ctx );    // my link but his object (CLIP_MODEL)
    } else {
        // THE COMP RESULT, SEGMENT BY SEGMENT (plan 53 §4.4). Per run the take
        // that SOUNDS there, drawn in its own domain and clipped to the run; a
        // run whose take has no material is hatched. This is what closes X10:
        // before it, a comped column drew the ACTIVE take across the whole
        // clip, so the picture showed material the column does not play and
        // hid material it does.
        //
        // GATED SINCE P7 (QBX-157) — and it is worth knowing what it took,
        // because for one whole ticket it was not. A0b shipped this branch with
        // a note saying that reverting it to the old active-take-only draw
        // passed the WHOLE suite, 449/449: this is pure PAINT, and a paint
        // behaviour needs a PIXEL gate. `collectEnvelope` below was gated
        // (`assert-envelope`, clip and `take=`) and the two share ONE run walk
        // (`compRuns`) so they cannot drift — but that is an argument, not a
        // gate. `take_comp_backdrop.qxa` is the gate: it grabs the composite
        // lane and asserts the comp result's SHAPE on it (a hole where the
        // selected take has no material, at 25–75 % of a column whose active
        // take covers all of it), and the same three quarters as ENERGY through
        // the audio path, on one fixture.
        const offset_t base = lk.getStartTime();
        runs = compRuns( st, vr.width(),
                         [&ctx, &vr, base]( int i ) -> offset_t {
                             return ctx.getTimeOf( vr.left() + i ) - base;
                         } );
        for( const CompRun &r : runs ) {
            const QRect rr( vr.left() + r.c0, vr.y(), r.c1 - r.c0, vr.height() );
            if( rr.width() < 1 ) continue;
            if( r.take < 0 ) {
                p.fillRect( rr, noMaterialBrush() );
                continue;
            }
            SObject *take = st.takeObjectAt( r.take );
            SObjectRenderer *rndr = take ? take->getInlineRenderer() : nullptr;
            if( !rndr ) continue;
            TakeShiftContext shifted( ctx, r.tStart );
            p.save();
            p.setClipRect( rr, Qt::IntersectClip );
            rndr->draw( lk, shifted );
            p.restore();
        }
    }

    // AFTER the content and BEFORE the badge: a mark annotates the material, so
    // it goes on top of it -- and the "no take" column has no cut in it to show
    // (its whole extent is the absence the hatch already states).
    if( st.activeTakeObject() || st.nTakes() > 0 )
        drawCutMarks( p, vr, ctx, st, lk, runs );

    if( st.nTakes() > 1 ) {
        p.setPen( QColor( 240, 220, 80 ) );
        p.drawText( vr.adjusted( 2, 0, 0, 0 ),
                    Qt::AlignTop | Qt::AlignLeft,
                    QStringLiteral( "%1/%2" )
                        .arg( st.activeTakeIndex() + 1 )
                        .arg( st.nTakes() ) );
    }
}

// The COLLECT terminal (proposal 39 M1), and since 1' the COMP RESULT's — the
// same run walk draw() makes, with a collect terminal instead of a draw one, so
// the envelope a script reads is the envelope on screen. No take, no envelope:
// the "no take" hatch has no waveform to describe.
bool STakeStackRendererInline::collectEnvelope( SLink &lk,
                                                const SEnvelopeWindow &win,
                                                preview_t *out )
{
    STakeStack &st = stack();
    if( !st.resolvesThroughComp() ) {
        // The degenerate column: the active take's own envelope, unchanged.
        SObject *take = st.activeTakeObject();
        if( !take ) return false;
        SObjectRenderer *rndr = take->getInlineRenderer();
        if( !rndr ) return false;
        return rndr->collectEnvelope( lk, win, out );
    }
    if( st.nTakes() == 0 ) return false;

    const int w = win.width < 1 ? 1 : win.width;
    for( int i = 0; i < w; ++i ) { out[ i ].min = 0; out[ i ].max = 0; }

    const offset_t base = lk.getStartTime();
    // Linear BY CONTRACT (sobjectrenderer.h), the same grid collectTakeEnvelope
    // interpolates.
    const auto posOf = [&win, w]( int i ) -> offset_t {
        const int64_t span = (int64_t) win.rightTime - (int64_t) win.leftTime;
        return (offset_t) ( (int64_t) win.leftTime
                            + ( span * (int64_t) i ) / (int64_t) w );
    };
    const std::vector<CompRun> runs =
        compRuns( st, w, [&posOf, base]( int i ) -> offset_t
                         { return posOf( i ) - base; } );

    bool any = false;
    for( const CompRun &r : runs ) {
        if( r.take < 0 ) continue;          // no material: the zeros stand
        SObject *take = st.takeObjectAt( r.take );
        SObjectRenderer *rndr = take ? take->getInlineRenderer() : nullptr;
        if( !rndr ) continue;
        SEnvelopeWindow sub;
        sub.leftTime  = posOf( r.c0 ) - r.tStart;
        sub.rightTime = ( r.c1 < w ? posOf( r.c1 ) : (offset_t) win.rightTime )
                        - r.tStart;
        sub.width     = r.c1 - r.c0;
        if( sub.width < 1 ) continue;
        if( rndr->collectEnvelope( lk, sub, out + r.c0 ) ) any = true;
    }
    // TRUE once ANY run had an envelope: the array describes the comp result,
    // zeros and all. False only when nothing in view could describe itself,
    // which is the "unknown material" case the caller must not read as "no
    // material" (an all-event column).
    return any;
}
