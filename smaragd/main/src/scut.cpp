
#include <iostream>
#include <math.h>

#include <QDebug>

#include "twcomponent.h"
#include "twrandomsource.h"
#include "twsamplereader.h"
#include "twloopreader.h"
#include "twgrainsource.h"
#include "twcapturingsource.h"
#include "sapplication.h"
#include "sproject.h"
#include "scut.h"
#include "slink.h"
#include "scutrndrinline.h"
#include "sprojectloader.h"

using namespace std;

void SCut::ensureReader()
{
    if( readerTried_ ) return;
    rebuildReader();
}

// (Re)build the playback chain: content source -> optional grain stretch ->
// our own cursor. Called lazily on first pull, and eagerly from setGrainParams
// (on the UI thread) so the one-time grain materialisation never lands in a
// realtime audio block. NB: rebuilding while this clip is actively playing is
// not yet thread-safe — set parameters while stopped (proposal 06 MVP).
void SCut::rebuildReader()
{
    readerTried_ = true;
    if( reader_ ) { delete reader_; reader_ = NULL; }
    if( grain_ )  { delete grain_;  grain_  = NULL; }

    twRandomSource *rs = content_->getSObject().getRandomSource();
    if( !rs ) rs = ensureCapture();
    if( !rs ) return;   // non-source, non-capturable: getRootComponent() falls back

    twRandomSource *view = rs;
    if( !grainParams_.isIdentity() ) {
        grain_ = new twGrainSource( *rs, grainParams_ );
        view = grain_;
    }
    tw303aEnvironment &env = *(SApplication::app().get303aEnvironment());
    if( loopLength_ > 0 && loopLength_ < cutDuration_ ) {
        // Loop active: wrap the view in a looping reader over the segment
        // [startOffset_, startOffset_+loopLength_). It is a twSampleReader, so it
        // lives in reader_ like any other; looping_ tells seekTo() not to add
        // startOffset_ again (the loop reader adds its loop base itself).
        twLoopReader *lr = new twLoopReader( env, *view, startOffset_, loopLength_ );
        lr->init();
        reader_ = lr;
        looping_ = true;
    } else {
        reader_ = view->acquireReader( env );
        looping_ = false;
    }
}

// Render a container content (a track/mixer sub-arrangement) once into an owned
// twCapturingSource, so each placement reads a cheap, independent snapshot rather
// than re-pulling — and fighting the single cursor of — the live graph (proposal
// 07 step 5). The whole content [0, dur) is captured (the cut's startOffset_
// indexes into it exactly like a sample source), so two cuts windowing the same
// container still each get a correct view. Returns NULL for real sources (the
// sample path) or content we cannot capture.
twRandomSource *SCut::ensureCapture()
{
    if( capture_ ) return capture_;
    SObject &c = content_->getSObject();
    if( c.getRandomSource() ) return NULL;          // real source -> sample path
    if( !c.hasDuration() ) return NULL;
    twComponent &comp = c.getRootComponent();
    if( !comp.isSeekable() ) return NULL;           // can't capture reliably
    length_t need = (length_t) startOffset_ + cutDuration_;
    length_t dur  = (length_t) c.getDuration();
    length_t n = dur > need ? dur : need;
    if( n <= 0 ) return NULL;

    tw303aEnvironment &env = *(SApplication::app().get303aEnvironment());
    capture_ = new twCapturingSource( env, comp, 0, n, 1, env.getSRate() );

    if( !captureConnected_ ) {
        // Transparent invalidation: any applied action drops the snapshot so the
        // next pull re-captures the edited arrangement.
        QObject::connect( &getProject(), SIGNAL( arrangementChanged() ),
                          this, SLOT( invalidateCapture() ) );
        captureConnected_ = true;
    }
    return capture_;
}

void SCut::invalidateCapture()
{
    // Drop the cached render and anything built over it; the next pull
    // re-captures. NB: like rebuildReader(), this is not realtime-safe — in the
    // MVP edits/auditions happen while stopped.
    if( reader_ )  { delete reader_;  reader_  = NULL; }
    if( grain_ )   { delete grain_;   grain_   = NULL; }
    if( capture_ ) { delete capture_; capture_ = NULL; }
    readerTried_ = false;
}

twComponent &SCut::getRootComponent()
{
    ensureReader();
    if( reader_ ) return *reader_;
    // Content is not a random-access source: fall back to its shared component.
    return content_->getRootComponent();
}

QWidget *SCut::getDetailEditWidget( QWidget * )
{
    return NULL;
}

QWidget *SCut::getInlineEditWidget( QWidget * )
{
    return NULL;
}

SObjectRenderer *SCut::getInlineRenderer( void ) 
{
    if( !inlineRenderer_ ) {
        inlineRenderer_ = new SCutRendererInline( *this );
    }
    return inlineRenderer_;
}

int SCut::seekTo( offset_t off )
{
    // FIXME: bounds check!!!
    ensureReader();
    // A loop reader is cut-relative (it adds its own loop base = startOffset_);
    // a plain reader needs startOffset_ folded in here.
    if( reader_ ) return reader_->seekTo( looping_ ? off : off + startOffset_ );
    return content_->getSObject().seekTo( off+startOffset_ );
}

void SCut::setStartOffset( offset_t off )
{
    startOffset_ = off;
}

void SCut::setDuration( length_t dur )
{
    // FIXME: clip.
    cutDuration_ = dur;
    emit durationChanged( dur );
}

void SCut::setLoopStart( offset_t s )
{
    loopStart_ = s;
}

void SCut::setLoopLength( length_t l )
{
    loopLength_ = l;
    rebuildReader();   // loop on/off changes the playback chain
    emit durationChanged( cutDuration_ );
}

void SCut::setWindow( offset_t startOffset, length_t duration,
                      length_t loopLength, double stretch )
{
    startOffset_ = startOffset;
    cutDuration_ = duration;
    loopLength_  = loopLength;
    grainParams_.stretch = stretch;
    rebuildReader();   // one rebuild for the whole window change (UI thread)
    emit durationChanged( cutDuration_ );
}

offset_t SCut::getLoopStart() const
{
    return loopStart_;
}

length_t SCut::getDuration() const
{
    return cutDuration_;
}

void SCut::setGrainParams( const twGrainParams &p )
{
    double oldStretch = grainParams_.stretch;
    grainParams_ = p;

    // Keep the clip covering the same musical span: stretching 2x makes the clip
    // twice as long on the timeline (and shifts its source window accordingly).
    if( oldStretch > 0.0 && p.stretch > 0.0 && p.stretch != oldStretch ) {
        double k = p.stretch / oldStretch;
        cutDuration_ = (length_t) llround( (double) cutDuration_ * k );
        startOffset_ = (offset_t) llround( (double) startOffset_ * k );
    }

    rebuildReader();   // pre-build off the audio thread (caller is the UI thread)
    emit durationChanged( cutDuration_ );
}

void SCut::setStretch( double s )
{
    twGrainParams p = grainParams_;
    p.stretch = s;
    setGrainParams( p );
}

void SCut::setPitchCents( double cents )
{
    twGrainParams p = grainParams_;
    p.pitchCents = cents;
    setGrainParams( p );
}

SCut::~SCut()
{
    // reader_ / grain_ only reference the (longer-lived) source data; drop them
    // before releasing the content link.
    if( reader_ ) { delete reader_; reader_ = NULL; }
    if( grain_ )  { delete grain_;  grain_  = NULL; }
    if( capture_ ) { delete capture_; capture_ = NULL; }
    delete content_;
    content_ = NULL;
}

SCut::SCut( SProject *parentProject, SLink &content )
    : SObject( parentProject ),
      startOffset_( 0 ),
      loopLength_( 0 ),
      inlineRenderer_( NULL ),
      reader_( NULL ),
      grain_( NULL ),
      looping_( false ),
      readerTried_( false )
{
    content_ = &content;
    content_->setParent(this);
    /* was:
    if( content_->parent() ) {
        content_->parent()->removeChild( content_ );
    }
    insertChild( content_ );
    */
    if( content_->getSObject().hasDuration() ) {
        cutDuration_ = content_->getSObject().getDuration();
    } else {
        // FIXME: remove 44100
        cutDuration_ = 22050;
    }
}

SCut::SCut( SProject *parentProject, SObject &content )
    : SObject( parentProject ),
      startOffset_( 0 ),
      loopLength_( 0 ),
      inlineRenderer_( NULL ),
      reader_( NULL ),
      grain_( NULL ),
      looping_( false ),
      readerTried_( false )
{
    content_ = new SLink( content, this );
    if( content_->getSObject().hasDuration() ) {
        cutDuration_ = content_->getSObject().getDuration();
    } else {
        // FIXME: remove 44100
        cutDuration_ = 22050;
    }
    // Loop start defaults to no loop.
    loopStart_ = cutDuration_;
}

int SCut::serializeSelfAttributes( QTextStream &o )
{
    o << " startOffset='" << (unsigned long ) getStartOffset() << "'"
      << " cutDuration='" << (unsigned long ) cutDuration_ << "'"
      << " loopLength='" << (unsigned long ) loopLength_ << "'"
      << " stretch='" << grainParams_.stretch << "'"
      << " pitchCents='" << grainParams_.pitchCents << "'"
      << " grainSize='" << (unsigned long ) grainParams_.grainSize << "'"
      << " crossfade='" << (unsigned long ) grainParams_.crossfade << "'";
    SObject::serializeSelfAttributes( o );
    return 0;
}

int SCut::readPostChildrenAttributes( QDomElement &element )
{
    SObject::readPostChildrenAttributes( element );

    QString data;
    data = element.attribute( "startOffset", "0" );
    setStartOffset( data.toULongLong() );
    data = element.attribute( "cutDuration", "44100" );
    cutDuration_ = data.toULongLong();
    loopLength_ = element.attribute( "loopLength", "0" ).toULongLong();

    // Grain params are stored already-final, so set them directly (no rescale).
    grainParams_.stretch    = element.attribute( "stretch", "1.0" ).toDouble();
    grainParams_.pitchCents = element.attribute( "pitchCents", "0.0" ).toDouble();
    grainParams_.grainSize  = element.attribute( "grainSize", "2048" ).toLongLong();
    grainParams_.crossfade  = element.attribute( "crossfade", "512" ).toLongLong();

    // Pre-build the playback chain now (load thread) so a stretched or looping
    // clip restored from disk does not materialise on the first realtime block.
    if( !grainParams_.isIdentity() || ( loopLength_ > 0 && loopLength_ < cutDuration_ ) )
        rebuildReader();

    return 0;
}

SLink *SCut::instantiateFromDomElement( 
    SProjectLoader &projectLoader, QDomElement &element, SObject *parent )
{
    SLink *contentLink = NULL;
    // Find the first link child 
    QDomNode childNode = element.firstChild();
    while( !childNode.isNull() ) {
        if( childNode.isElement() ) {
            qWarning() << "found SCut child " << childNode.nodeName() << Qt::endl;
            if( childNode.nodeName() == "SLink" ) {
                QDomElement childElement = childNode.toElement();
                QString objectId = childElement.attribute( "objectId" );
                // Look up the object id.
                contentLink = projectLoader.getObjectDictionary().value( objectId );
                if( contentLink ) break;
            }
        }
        childNode = childNode.nextSibling();
    }
                
    if( !contentLink ) {
        qWarning( "SCut did not have a child!!" );
        return NULL;
    }
    SCut *cut = new SCut( &projectLoader.getProject(), contentLink->getSObject() );
    cut->readPreChildrenAttributes( element );
    // Now read out the properties.
    cut->readPostChildrenAttributes( element );

    return new SLink( *cut, parent );
}
