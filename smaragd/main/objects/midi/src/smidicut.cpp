#include "app/objects/midi/smidicut.h"
#include "app/model/sautomationlane.h"

#include <limits>

#include <QDebug>
#include <QDomElement>
#include <QTextStream>
#include <algorithm>
#include <string>

#include "app/model/slink.h"
#include "app/model/sproject.h"
#include "app/objects/midi/smidirndrinline.h"
#include "app/objects/midi/smidisequence.h"
#include "app/persistence/sprojectloader.h"
#include "tw/events/twtempomap.h"

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

SMidiCut::SMidiCut( SProject *project, SObject &content )
    : SObject( project )
{
    // Own content link, parent set LAST (slink.h rule) - identical to SCut.
    content_ = new SLink( content, NULL );
    content_->setParent( this );

    if( SMidiSequence *seq = dynamic_cast<SMidiSequence *>( &content ) ) {
        const qint64 len = seq->lengthTicks();
        lengthTicks_ = Fraction( len > 0 ? len : SMidiSequence::DEFAULT_PPQ * 4 );
    } else {
        lengthTicks_ = Fraction( SMidiSequence::DEFAULT_PPQ * 4 );  // one 4/4 bar
    }

    QObject::connect( &content, SIGNAL( eventsChanged( offset_t ) ),
                      this, SLOT( onContentEventsChanged( offset_t ) ) );
    if( project ) {
        // Every frame-facing value here is DERIVED from ticks through the map,
        // so both of these move it. Ticks themselves never change.
        QObject::connect( project, SIGNAL( bpmTempoChanged( double ) ),
                          this, SLOT( onTempoOrRateChanged() ) );
        QObject::connect( project, SIGNAL( sampleRateChanged( int ) ),
                          this, SLOT( onTempoOrRateChanged() ) );
    }

    std::lock_guard<std::mutex> lock( mutex() );
    rebuild_nolock();
}

SMidiCut::~SMidiCut()
{
    delete content_;
    content_ = nullptr;
}

SObject &SMidiCut::getContent() const
{
    return content_->getSObject();
}

SMidiSequence *SMidiCut::sequence() const
{
    return content_ ? dynamic_cast<SMidiSequence *>( &content_->getSObject() )
                    : nullptr;
}

// ---------------------------------------------------------------------------
// Snapshot / derivation - THE one tick -> frame conversion site
// ---------------------------------------------------------------------------

void SMidiCut::rebuild_nolock()
{
    SProject *project = getProjectSafe();
    snapshot_.srcStartTicks = srcStartTicks_;
    snapshot_.lengthTicks   = lengthTicks_;
    snapshot_.loopTicks     = loopTicks_;
    snapshot_.rate          = rate_;

    if( !project ) {
        snapshot_.durationFrames = 0;
        snapshot_.loopFrames = 0;
        snapshot_.startOffsetFrames = 0;
        snapshot_.framesSeq.reset();
        return;
    }
    const twTempoMap &map = project->tempoMap();
    const int srate = project->getSRate();
    // Frames per ONE tick, exact. Everything below is this factor times an
    // exact tick value, floored once at the render boundary - the same shape
    // as SCut's floor(srcStart * stretch).
    const Fraction framesPerTick = map.ticksToFrames( TickLen( (int64_t) 1 ), srate );

    snapshot_.durationFrames =
        (length_t) ( lengthTicks_ * framesPerTick ).floorToInt();
    snapshot_.loopFrames =
        (length_t) ( loopTicks_ * framesPerTick ).floorToInt();
    // The window's start expressed in the frame-domain sequence below: the
    // content anchor scaled by the rate, exactly as SCut derives its warped
    // startOffset from srcStart * stretch.
    snapshot_.startOffsetFrames =
        (offset_t) ( srcStartTicks_ * rate_ * framesPerTick ).floorToInt();

    std::vector<SEvent> events;
    if( SMidiSequence *seq = sequence() ) events = seq->events();
    // The lanes are read HERE, under the cut's own mutex, so a rebuild and an
    // edit cannot interleave; the snapshot they produce is immutable and is what
    // every consumer sees.
    const std::shared_ptr<const twAutomationCurve> transCurve =
        automationCurve( QStringLiteral( "cut:Transpose" ) );
    const std::shared_ptr<const twAutomationCurve> velCurve =
        automationCurve( QStringLiteral( "cut:VelocityScale" ) );
    snapshot_.framesSeq = smidievents::buildSeq(
        events, rate_ * framesPerTick, transpose_, velocityScale_,
        channelOverride_, transCurve.get(), velCurve.get() );
}

// --- automation (proposal 37 P5) --------------------------------------------

void SMidiCut::applyAutomationToEngine()
{
    {
        std::lock_guard<std::mutex> lock( mutex() );
        rebuild_nolock();
    }
    // Open-ended: the consumer of an event stream is class-1 (design F9), so an
    // event change is never bounded on the right.
    emit eventsChanged( 0 );
}

void SMidiCut::onAutomationChanged( SAutomationLane &lane, offset_t start, offset_t end )
{
    (void) end;
    const bool eventLane = ( lane.ref().space == SParamRef::Space::Cut
                             && lane.ref().prop != QLatin1String( "Gain" ) );
    if( eventLane ) {
        applyAutomationToEngine();
        // The track turns eventsChanged into its own open-ended invalidation;
        // this is the walk for the paths that do not go through the feed.
        invalidateRenderPathRange( start, std::numeric_limits<offset_t>::max() );
        return;
    }
    SObject::onAutomationChanged( lane, start, end );
}

void SMidiCut::publish_( length_t oldDuration )
{
    const length_t now = getDuration();
    if( now != oldDuration ) emit durationChanged( now );
    // Open-ended by design: the consumer of an event stream is class-1, so an
    // edit is never bounded on the right (design 3.2, F9). The track turns
    // this into touchClip + invalidateRenderPathRange(from, INF).
    emit eventsChanged( 0 );
}

SMidiCutSnapshot SMidiCut::getSnapshot() const
{
    std::lock_guard<std::mutex> lock( mutex() );
    return snapshot_;
}

Fraction SMidiCut::getSrcStartTicks() const
{
    std::lock_guard<std::mutex> lock( mutex() );
    return srcStartTicks_;
}
Fraction SMidiCut::getLengthTicks() const
{
    std::lock_guard<std::mutex> lock( mutex() );
    return lengthTicks_;
}
Fraction SMidiCut::getLoopTicks() const
{
    std::lock_guard<std::mutex> lock( mutex() );
    return loopTicks_;
}
Fraction SMidiCut::getRateExact() const
{
    std::lock_guard<std::mutex> lock( mutex() );
    return rate_;
}
int SMidiCut::getTranspose() const
{
    std::lock_guard<std::mutex> lock( mutex() );
    return transpose_;
}
double SMidiCut::getVelocityScale() const
{
    std::lock_guard<std::mutex> lock( mutex() );
    return velocityScale_;
}
int SMidiCut::getChannelOverride() const
{
    std::lock_guard<std::mutex> lock( mutex() );
    return channelOverride_;
}

length_t SMidiCut::getDuration() const
{
    std::lock_guard<std::mutex> lock( mutex() );
    return snapshot_.durationFrames;
}

length_t SMidiCut::loopLength() const
{
    std::lock_guard<std::mutex> lock( mutex() );
    return snapshot_.loopFrames;
}

offset_t SMidiCut::startOffset() const
{
    std::lock_guard<std::mutex> lock( mutex() );
    return snapshot_.startOffsetFrames;
}

// ---------------------------------------------------------------------------
// Window writes. Every one of them takes TIMELINE FRAMES and converts here.
// ---------------------------------------------------------------------------

void SMidiCut::setWindowTicks( const Fraction &srcStartTicks,
                               const Fraction &lengthTicks,
                               const Fraction &loopTicks, const Fraction &rate )
{
    const length_t oldDuration = getDuration();
    {
        std::lock_guard<std::mutex> lock( mutex() );
        srcStartTicks_ = srcStartTicks;
        lengthTicks_   = lengthTicks < Fraction( 0 ) ? Fraction( 0 ) : lengthTicks;
        loopTicks_     = loopTicks   < Fraction( 0 ) ? Fraction( 0 ) : loopTicks;
        rate_          = rate > Fraction( 0 ) ? rate : Fraction( 1 );
        rebuild_nolock();
    }
    publish_( oldDuration );
}

// frames -> TIMELINE ticks, exactly once, right here.
static Fraction framesToTicksExact( SProject *project, int64_t frames )
{
    if( !project ) return Fraction( 0 );
    return project->tempoMap()
        .framesToTickLen( frames, project->getSRate() )
        .ticks();
}

void SMidiCut::setDuration( length_t d )
{
    SProject *project = getProjectSafe();
    setWindowTicks( getSrcStartTicks(),
                    framesToTicksExact( project, (int64_t) d ),
                    getLoopTicks(), getRateExact() );
}

void SMidiCut::setStartOffsetFromTimeline( offset_t startOffset )
{
    SProject *project = getProjectSafe();
    // The offset is read in the frame domain the SEQUENCE lives in (content
    // ticks scaled by the rate), so dividing by the rate lands on the content
    // anchor - the mirror image of SCut's warpedToSrc.
    const Fraction rate = getRateExact();
    Fraction anchor = framesToTicksExact( project, (int64_t) startOffset );
    if( rate > Fraction( 0 ) ) anchor = anchor / rate;
    setWindowTicks( anchor, getLengthTicks(), getLoopTicks(), rate );
}

void SMidiCut::setWindowFromTimeline( offset_t startOffset, length_t duration,
                                      length_t loopLength,
                                      const Fraction &stretchOrRate )
{
    SProject *project = getProjectSafe();
    const Fraction rate =
        stretchOrRate > Fraction( 0 ) ? stretchOrRate : Fraction( 1 );
    Fraction anchor = framesToTicksExact( project, (int64_t) startOffset ) / rate;
    setWindowTicks( anchor,
                    framesToTicksExact( project, (int64_t) duration ),
                    framesToTicksExact( project, (int64_t) loopLength ), rate );
}

void SMidiCut::setWindowExact( const Fraction &contentAnchor, length_t duration,
                               length_t loopLength,
                               const Fraction &stretchOrRate )
{
    SProject *project = getProjectSafe();
    setWindowTicks( contentAnchor,
                    framesToTicksExact( project, (int64_t) duration ),
                    framesToTicksExact( project, (int64_t) loopLength ),
                    stretchOrRate > Fraction( 0 ) ? stretchOrRate
                                                  : Fraction( 1 ) );
}

void SMidiCut::setContentAnchorExact( const Fraction &contentAnchor )
{
    // RAW: no publish. Used while a freshly created window is still unplaced
    // (SClipWindow's documented exception).
    std::lock_guard<std::mutex> lock( mutex() );
    srcStartTicks_ = contentAnchor;
    rebuild_nolock();
}

Fraction SMidiCut::timelineToSourceExact( const Fraction &relTimeline ) const
{
    SProject *project = getProjectSafe();
    if( !project ) return getSrcStartTicks();
    // relTimeline is in TIMELINE FRAMES (SClipWindow rule 1). One conversion:
    // frames -> timeline ticks -> content ticks (divide by the rate) -> anchor.
    const Fraction rate = getRateExact();
    const Fraction ticks = project->tempoMap()
        .framesToTickLen( relTimeline.floorToInt(), project->getSRate() )
        .ticks();
    return getSrcStartTicks() + ( rate > Fraction( 0 ) ? ticks / rate : ticks );
}

// ---------------------------------------------------------------------------
// Modifiers
// ---------------------------------------------------------------------------

void SMidiCut::setTranspose( int semitones )
{
    const int t = clampTranspose( semitones );
    {
        std::lock_guard<std::mutex> lock( mutex() );
        if( transpose_ == t ) return;
        transpose_ = t;
        rebuild_nolock();
    }
    emit eventsChanged( 0 );
}

void SMidiCut::setVelocityScale( double scale )
{
    if( scale < 0.0 ) scale = 0.0;
    if( scale > 8.0 ) scale = 8.0;
    {
        std::lock_guard<std::mutex> lock( mutex() );
        if( velocityScale_ == scale ) return;
        velocityScale_ = scale;
        rebuild_nolock();
    }
    emit eventsChanged( 0 );
}

void SMidiCut::setChannelOverride( int channel )
{
    const int c = ( channel < 0 || channel > 15 ) ? -1 : channel;
    {
        std::lock_guard<std::mutex> lock( mutex() );
        if( channelOverride_ == c ) return;
        channelOverride_ = c;
        rebuild_nolock();
    }
    emit eventsChanged( 0 );
}

void SMidiCut::setRate( double rate )
{
    if( !( rate > 0.0 ) ) return;
    setWindowTicks( getSrcStartTicks(), getLengthTicks(), getLoopTicks(),
                    parseFractionOrDouble( std::to_string( rate ) ) );
}

void SMidiCut::onTempoOrRateChanged()
{
    const length_t oldDuration = getDuration();
    {
        std::lock_guard<std::mutex> lock( mutex() );
        rebuild_nolock();
    }
    publish_( oldDuration );
}

void SMidiCut::onContentEventsChanged( offset_t )
{
    const length_t oldDuration = getDuration();
    {
        std::lock_guard<std::mutex> lock( mutex() );
        rebuild_nolock();
    }
    publish_( oldDuration );
}

// ---------------------------------------------------------------------------
// SObject
// ---------------------------------------------------------------------------

std::shared_ptr<twComponent> SMidiCut::getRootComponent()
{
    // Silence, from the content (M5): an event clip is never routed through a
    // bus mixer, but a container capture or an asset can still ask.
    return getContent().getRootComponent();
}

QWidget *SMidiCut::getDetailEditWidget( QWidget * ) { return nullptr; }
QWidget *SMidiCut::getInlineEditWidget( QWidget * ) { return nullptr; }

SObjectRenderer *SMidiCut::getInlineRenderer()
{
    if( !inlineRenderer_ ) inlineRenderer_ = new SMidiCutRendererInline( *this );
    return (SObjectRenderer *) inlineRenderer_;
}

twEventClipResolved SMidiCut::resolveEventClip( offset_t )
{
    // ONE snapshot for the whole collect (events/CONTRACT inv. 10, the same
    // coherence rule twView::resolve gives the audio path).
    SMidiCutSnapshot snap = getSnapshot();
    twEventClipResolved out;
    out.seq = snap.framesSeq;
    // Loop and slip go through the standard maps, so every caller spells the
    // same semantics (POSITION_DOMAINS rules 1-3).
    if( snap.loopFrames > 0 && snap.loopFrames < snap.durationFrames )
        out.map = twEventLoopMap( snap.startOffsetFrames, snap.loopFrames );
    else
        out.map = twEventSlipMap( snap.startOffsetFrames );
    return out;
}

SClipWindow *SMidiCut::cloneWindowOver( SProject *project ) const
{
    SMidiCut *copy = new SMidiCut( project, getContent() );
    copy->setWindowTicks( getSrcStartTicks(), getLengthTicks(), getLoopTicks(),
                          getRateExact() );
    copy->setTranspose( getTranspose() );
    copy->setVelocityScale( getVelocityScale() );
    copy->setChannelOverride( getChannelOverride() );
    // The clip's automation lanes are part of the WINDOW (design 3.3), so a
    // duplicate or a new take carries them exactly as it carries the transpose.
    copy->copyAutomationFrom( *this );
    return copy;
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

int SMidiCut::serializeSelfAttributes( QTextStream &o )
{
    // Snapshot first, WRITE second. The base class's serializer calls
    // getDuration(), which takes mutex() - holding it across that call is a
    // self-deadlock (std::mutex is not recursive), and it is silent: the save
    // simply never finishes.
    Fraction srcStartTicks, lengthTicks, loopTicks, rate;
    int transpose; double velocityScale; int channelOverride;
    {
        std::lock_guard<std::mutex> lock( mutex() );
        srcStartTicks = srcStartTicks_;
        lengthTicks = lengthTicks_;
        loopTicks = loopTicks_;
        rate = rate_;
        transpose = transpose_;
        velocityScale = velocityScale_;
        channelOverride = channelOverride_;
    }
    o << " srcStartTicks='"
      << QString::fromStdString( srcStartTicks.toString() ) << "'"
      << " lengthTicks='"
      << QString::fromStdString( lengthTicks.toString() ) << "'"
      << " loopTicks='"
      << QString::fromStdString( loopTicks.toString() ) << "'"
      << " rate='" << QString::fromStdString( rate.toString() ) << "'";
    if( transpose != 0 ) o << " transpose='" << transpose << "'";
    if( velocityScale != 1.0 ) o << " velocityScale='" << velocityScale << "'";
    if( channelOverride >= 0 ) o << " channel='" << channelOverride << "'";
    SObject::serializeSelfAttributes( o );
    return 0;
}

int SMidiCut::readPostChildrenAttributes( QDomElement &element )
{
    SObject::readPostChildrenAttributes( element );
    {
        std::lock_guard<std::mutex> lock( mutex() );
        // The tick values are the AUTHORITY and are read last: they discard
        // whatever the base class's `durationSec` migration computed, which is
        // a rate-scaled frame count and therefore meaningless for a tick-native
        // window (D2 - ticks are rate-free, so there is no migration to do).
        srcStartTicks_ = parseFractionOrDouble(
            element.attribute( "srcStartTicks", "0" ).toStdString() );
        lengthTicks_ = parseFractionOrDouble(
            element.attribute( "lengthTicks", "0" ).toStdString() );
        loopTicks_ = parseFractionOrDouble(
            element.attribute( "loopTicks", "0" ).toStdString() );
        rate_ = parseFractionOrDouble(
            element.attribute( "rate", "1" ).toStdString() );
        if( !( rate_ > Fraction( 0 ) ) ) rate_ = Fraction( 1 );
        transpose_ = clampTranspose( element.attribute( "transpose", "0" ).toInt() );
        velocityScale_ = element.attribute( "velocityScale", "1" ).toDouble();
        if( !( velocityScale_ >= 0.0 ) ) velocityScale_ = 1.0;
        channelOverride_ = element.attribute( "channel", "-1" ).toInt();
        if( channelOverride_ < 0 || channelOverride_ > 15 ) channelOverride_ = -1;
        rebuild_nolock();
    }
    return 0;
}

SLink *SMidiCut::instantiateFromDomElement(
    SProjectLoader &projectLoader, QDomElement &element, SObject *parent )
{
    SLink *contentLink = NULL;
    QDomNode childNode = element.firstChild();
    while( !childNode.isNull() ) {
        if( childNode.isElement() && childNode.nodeName() == "SLink" ) {
            QDomElement childElement = childNode.toElement();
            contentLink = projectLoader.getObjectDictionary().value(
                childElement.attribute( "objectId" ) );
            if( contentLink ) break;
        }
        childNode = childNode.nextSibling();
    }
    if( !contentLink ) {
        qWarning() << "SMidiCut has no resolvable content link";
        return NULL;
    }
    SMidiCut *cut = new SMidiCut( &projectLoader.getProject(),
                                  contentLink->getSObject() );
    cut->readPreChildrenAttributes( element );
    cut->readPostChildrenAttributes( element );

    SLink *cutLink = new SLink( *cut, NULL );
    if( parent ) cutLink->setParent( parent );
    return cutLink;
}

// The Event window type (D8b): SClipWindow::wrapContent( project, <event
// content> ) mints an SMidiCut, so `place-clip`, `split-clip`'s wrap path and
// every other windowed verb reach it without app/model naming a concrete type.
static const bool s_registered_smidicut_wrap = (
    SClipWindow::registerWrapFactory(
        SContentKind::Event,
        []( SProject *project, SObject &content ) -> SClipWindow * {
            return new SMidiCut( project, content );
        } ), true );

// A WINDOW: an SMidiCut whose content link is dead has nothing left to show,
// so the loader drops the cut itself rather than the track carrying it (D8a).
static const bool s_registered_smidicut =
    ( SProjectLoader::registerSObjectClass( "SMidiCut",
          SMidiCut::instantiateFromDomElement,
          SElementKind::Window ), true );
