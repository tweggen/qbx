#include "app/objects/cut/stakestack.h"
#include <utility>
#include <vector>
#include "app/model/sclipwindow.h"
#include "app/model/slink.h"
#include "app/model/sproject.h"
#include "app/model/sappcontext.h"
#include "app/persistence/sprojectloader.h"
#include "tw/graph/twcomponent.h"
#include "tw/core/twfraction.h"

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
    // ... then the notification, in the SAME order and by the SAME route
    // `setActiveTake` uses (proposal 15 ordering): the track's slot runs
    // updateClip -- a content-epoch bump and a state-chain reset, because our
    // component identity just changed -- and invalidateRenderPath. N1 has no
    // consumer, so this publishes a change nothing hears yet; wiring it now is
    // what means N2 adds a component and no plumbing.
    emit durationChanged( getDurationBlocking() );
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
    // STILL UNSHIFTED BY THE EXTENT, and that is P3's row rather than an
    // oversight: plan 53 §4.1 says so outright ("resolveEventClip is P3's and
    // is extent-shifted there"). An EVENT column with a partial take needs the
    // record itself cut to the extent, not just its position moved, and the
    // event path has no twCompColumn to do that in — which is the whole of
    // what MIDI parity (§8 P3, X16) is about.
    SObject *take = activeTakeObject();
    return take ? take->resolveEventClip( clipPos ) : twEventClipResolved{};
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
    SObject *take = activeTakeObject();
    return take ? take->hasPreview() : false;
}

int STakeStack::getPreview( preview_t *dest, offset_t start, length_t length,
                            offset_t nProbes )
{
    if( SObject *take = activeTakeObject() )
        return take->getPreview( dest, start, length, nProbes );
    return -1;
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
    if( SObject *take = takeFor( takeIndex ) )
        if( SObjectRenderer *rndr = take->getInlineRenderer() )
            rndr->draw( lk, ctx );          // my link but his object (CLIP_MODEL)
}

bool STakeStackRendererInline::collectTakeEnvelope( SLink &lk,
                                                    const SEnvelopeWindow &win,
                                                    preview_t *out,
                                                    int takeIndex )
{
    SObject *take = takeFor( takeIndex );
    if( !take ) return false;
    SObjectRenderer *rndr = take->getInlineRenderer();
    if( !rndr ) return false;
    return rndr->collectEnvelope( lk, win, out );
}

void STakeStackRendererInline::draw( SLink &lk, SRenderContext &ctx )
{
    QPainter &p = ctx.getPainter();
    const QRect vr = ctx.getVisibRect();
    STakeStack &st = stack();

    if( SObject *take = st.activeTakeObject() ) {
        if( SObjectRenderer *rndr = take->getInlineRenderer() )
            rndr->draw( lk, ctx );    // my link but his object (CLIP_MODEL)
    } else {
        p.fillRect( vr, QBrush( QColor( 70, 70, 70 ), Qt::BDiagPattern ) );
        p.setPen( QColor( 150, 150, 150 ) );
        p.drawText( vr, Qt::AlignCenter, QStringLiteral( "no take" ) );
    }

    if( st.nTakes() > 1 ) {
        p.setPen( QColor( 240, 220, 80 ) );
        p.drawText( vr.adjusted( 2, 0, 0, 0 ),
                    Qt::AlignTop | Qt::AlignLeft,
                    QStringLiteral( "%1/%2" )
                        .arg( st.activeTakeIndex() + 1 )
                        .arg( st.nTakes() ) );
    }
}

// The COLLECT terminal (proposal 39 M1). Identical delegation to draw()'s: the
// active take's renderer over MY link but HIS object (CLIP_MODEL). No take, no
// envelope — the "no take" hatch has no waveform to describe.
bool STakeStackRendererInline::collectEnvelope( SLink &lk,
                                                const SEnvelopeWindow &win,
                                                preview_t *out )
{
    SObject *take = stack().activeTakeObject();
    if( !take ) return false;
    SObjectRenderer *rndr = take->getInlineRenderer();
    if( !rndr ) return false;
    return rndr->collectEnvelope( lk, win, out );
}
