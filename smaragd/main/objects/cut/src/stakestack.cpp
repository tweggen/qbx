#include "app/objects/cut/stakestack.h"
#include "app/objects/cut/scut.h"
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

SCut *STakeStack::takeCutAt( int index ) const
{
    if( index < 0 || index >= childCount() ) return nullptr;
    SLink *lk = childAt( index );
    return lk ? dynamic_cast<SCut *>( &lk->getSObject() ) : nullptr;
}

SLink *STakeStack::insertTake( SCut &cut, int atIndex )
{
    SLink *lk = new SLink( cut, nullptr );
    lk->setStartTime( 0 );                     // takes are column-relative
    lk->setParent( this );                     // appends to childOrder_
    if( atIndex >= 0 && atIndex < childCount() - 1 ) {
        moveChildToIndex( childCount() - 1, atIndex );
        if( atIndex <= activeTake_ )
            ++activeTake_;                     // keep pointing at the same cut
    }
    QObject::connect( &cut, SIGNAL( durationChanged( length_t ) ),
                      this, SLOT( onTakeCutChanged( length_t ) ) );
    return lk;
}

void STakeStack::removeTake( int index )
{
    SLink *lk = childAt( index );
    if( !lk ) return;
    if( SCut *cut = takeCutAt( index ) ) {
        QObject::disconnect( cut, SIGNAL( durationChanged( length_t ) ),
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
    // to the root is re-frozen, siblings keep their caches).
    emit durationChanged( getDuration() );
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
    emit durationChanged( getDuration() );
}

void STakeStack::setDurationAll( length_t duration )
{
    forwardSuppressed_ = true;
    for( int i = 0; i < childCount(); ++i ) {
        if( SCut *cut = takeCutAt( i ) )
            cut->setDuration( duration );
    }
    forwardSuppressed_ = false;
    emit durationChanged( getDuration() );
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
        SCut *cut = takeCutAt( i );
        if( !cut ) continue;
        // Slip offsets live in the stretched OUTPUT domain: a stretch change
        // rescales them so every take keeps pointing at the same material.
        // The source anchor is authoritative (proposal 18 Phase 3): a
        // stretch change does not move it, so the old warped-offset rescale
        // (and its per-take rounding) is simply gone.
        cut->setWindow( cut->getSrcStart(), ClipLen( duration ),
                        WarpedLen( loopLength ), stretch );
    }
    forwardSuppressed_ = false;
    emit durationChanged( getDuration() );
}

void STakeStack::onTakeCutChanged( length_t )
{
    if( forwardSuppressed_ ) return;
    // Only the audible take's content reaches the mix; a slip/pitch edit on
    // it must resync the track (same window, changed content — updateClip
    // bumps the epoch unconditionally for exactly this case).
    SObject *obj = dynamic_cast<SObject *>( sender() );
    if( obj && static_cast<SObject *>( activeCut() ) == obj )
        emit durationChanged( getDuration() );
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
    if( SCut* cut = activeCut() )
        return cut->getRootComponent();
    return ensureSilence();
}

offset_t STakeStack::mapTimelineToComponentPos( offset_t off )
{
    if( SCut *cut = activeCut() )
        return cut->mapTimelineToComponentPos( off );
    return off;
}

int STakeStack::seekTo( offset_t off )
{
    if( SCut *cut = activeCut() )
        return cut->seekTo( off );
    return 0;
}

length_t STakeStack::getDuration() const
{
    // The window survives deactivation (activeTake == -1 plays silence but
    // the column keeps its extent), so fall back to the first take.
    if( const SCut *cut = activeCut() )
        return cut->getDuration();
    if( const SCut *cut = takeCutAt( 0 ) )
        return cut->getDuration();
    return 0;
}

bool STakeStack::hasPreview() const
{
    SCut *cut = activeCut();
    return cut ? cut->hasPreview() : false;
}

int STakeStack::getPreview( preview_t *dest, offset_t start, length_t length,
                            offset_t nProbes )
{
    if( SCut *cut = activeCut() )
        return cut->getPreview( dest, start, length, nProbes );
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
                if( SCut *cut = dynamic_cast<SCut *>(
                        &contentLink->getSObject() ) ) {
                    stack->insertTake( *cut );
                } else {
                    qWarning( "STakeStack: child %s is not an SCut, skipped",
                              qPrintable( objectId ) );
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
        "STakeStack", STakeStack::instantiateFromDomElement ), true );

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

void STakeStackRendererInline::draw( SLink &lk, SRenderContext &ctx )
{
    QPainter &p = ctx.getPainter();
    const QRect vr = ctx.getVisibRect();
    STakeStack &st = stack();

    if( SCut *cut = st.activeCut() ) {
        if( SObjectRenderer *rndr = cut->getInlineRenderer() )
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
