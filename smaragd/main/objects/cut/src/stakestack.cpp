#include "app/objects/cut/stakestack.h"
#include "app/model/sclipwindow.h"
#include "app/model/slink.h"
#include "app/model/sproject.h"
#include "app/model/sappcontext.h"
#include "app/persistence/sprojectloader.h"
#include "tw/graph/twcomponent.h"
#include "tw/core/twfraction.h"

#include <QDomElement>
#include <QPainter>
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

SLink *STakeStack::insertTake( SClipWindow &window, int atIndex )
{
    SObject &obj = window.asObject();
    if( childCount() > 0 && window.contentKind() != contentKind() ) {
        qWarning( "STakeStack: refusing a take of content kind %d into a "
                  "column of kind %d — a take is an ALTERNATIVE for the same "
                  "region, not a different instrument.",
                  (int) window.contentKind(), (int) contentKind() );
        return nullptr;
    }
    SLink *lk = new SLink( obj, nullptr );
    lk->setStartTime( 0 );                     // takes are column-relative
    lk->setParent( this );                     // appends to childOrder_
    if( atIndex >= 0 && atIndex < childCount() - 1 ) {
        moveChildToIndex( childCount() - 1, atIndex );
        if( atIndex <= activeTake_ )
            ++activeTake_;                     // keep pointing at the same take
    }
    QObject::connect( &obj, SIGNAL( durationChanged( length_t ) ),
                      this, SLOT( onTakeCutChanged( length_t ) ) );
    return lk;
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

void STakeStack::setDurationAll( length_t duration )
{
    forwardSuppressed_ = true;
    for( int i = 0; i < childCount(); ++i ) {
        if( SClipWindow *take = takeAt( i ) )
            take->setDurationFromTimeline( duration );
    }
    forwardSuppressed_ = false;
    // Emit the value we just set on every take — NOT getDuration(), which reads
    // the active take via SCut::getSnapshot()'s try-lock and can return the stale
    // pre-edit duration when a background revalidation worker holds the cut mutex.
    // That stale value reached the track (STrack::trackChildDurationChanged →
    // twTrackMix::updateClip), leaving a split clip's HEAD at its full pre-split
    // length so it bled into the tail region — the confirmed takes_group_broadcast
    // flake (proposal 19 Phase 2b). Emitting the authoritative value is race-free.
    emit durationChanged( duration );
}

void STakeStack::setDuration( length_t duration )
{
    setDurationAll( duration );
}

void STakeStack::applyWindowAll( length_t duration, length_t loopLength,
                                 const Fraction &stretch )
{
    forwardSuppressed_ = true;
    for( int i = 0; i < childCount(); ++i ) {
        SClipWindow *take = takeAt( i );
        if( !take ) continue;
        // Slip offsets live in the stretched OUTPUT domain: a stretch change
        // rescales them so every take keeps pointing at the same material.
        // The content anchor is authoritative (proposal 18 Phase 3): a
        // stretch change does not move it, so the old warped-offset rescale
        // (and its per-take rounding) is simply gone.
        take->setWindowExact( take->contentAnchorExact(), duration,
                              loopLength, stretch );
    }
    forwardSuppressed_ = false;
    // Authoritative value we just set on every take (proposal 19 Phase 2b) —
    // not a try-lock re-read that can return the stale pre-edit duration.
    emit durationChanged( duration );
}

void STakeStack::onTakeCutChanged( length_t )
{
    if( forwardSuppressed_ ) return;
    // Only the audible take's content reaches the mix; a slip/pitch edit on
    // it must resync the track (same window, changed content — updateClip
    // bumps the epoch unconditionally for exactly this case).
    SObject *obj = dynamic_cast<SObject *>( sender() );
    if( obj && activeTakeObject() == obj )
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

std::shared_ptr<twComponent> STakeStack::getRootComponent()
{
    if( SObject *take = activeTakeObject() )
        return take->getRootComponent();
    return ensureSilence();
}


QList<SObject::SDirtyRange> STakeStack::mapChildRangesToSelf(
    SLink *childLink, const QList<SDirtyRange> &childRanges )
{
    // Only the active take reaches the mix; an edit inside an inactive
    // take's content changes nothing audible (selecting that take later
    // goes through updateClip, which re-stales the column's extent).
    SObject *active = activeTakeObject();
    if( !childLink || !active || &childLink->getSObject() != active )
        return {};
    return SObject::mapChildRangesToSelf( childLink, childRanges );
}

twEventClipResolved STakeStack::resolveEventClip( offset_t clipPos )
{
    SObject *take = activeTakeObject();
    return take ? take->resolveEventClip( clipPos ) : twEventClipResolved{};
}

offset_t STakeStack::mapTimelineToComponentPos( offset_t off )
{
    if( SObject *take = activeTakeObject() )
        return take->mapTimelineToComponentPos( off );
    return off;
}

twResolvedClip STakeStack::resolveClip( offset_t off )
{
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
    if( SObject *take = activeTakeObject() )
        return take->seekTo( off );
    return 0;
}

length_t STakeStack::getDuration() const
{
    // The window survives deactivation (activeTake == -1 plays silence but
    // the column keeps its extent), so fall back to the first take.
    if( const SObject *take = activeTakeObject() )
        return take->getDuration();
    if( const SObject *take = takeObjectAt( 0 ) )
        return take->getDuration();
    return 0;
}

length_t STakeStack::getDurationBlocking() const
{
    // Blocking-snapshot duration for the edit/signal path (proposal 19 Phase 2b):
    // a durationChanged emit must carry the CURRENT duration, not the stale
    // try-lock value getDuration() can return under background-worker contention.
    if( const SObject *take = activeTakeObject() )
        return take->getDurationBlocking();
    if( const SObject *take = takeObjectAt( 0 ) )
        return take->getDurationBlocking();
    return 0;
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
    return 0;
}

int STakeStack::readPostChildrenAttributes( QDomElement &element )
{
    // The base call is not decoration: since proposal 37 P5 it is what reads
    // the inline <automation> child. A stack carries no lane of its own today
    // (a clip envelope lives on each TAKE), but an override that silently
    // dropped a known payload is exactly the trap the next owner would fall in.
    SObject::readPostChildrenAttributes( element );

    int at = element.attribute( "activeTake", "-1" ).toInt();
    if( at < -1 || at >= childCount() ) at = -1;
    activeTake_ = at;
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
                    // May be refused on a content-kind mismatch (see
                    // insertTake); that warns for itself and costs one take,
                    // never the column.
                    stack->insertTake( *take );
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
