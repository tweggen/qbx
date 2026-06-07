
#include <iostream>
#include <math.h>

#include <QDebug>

#include "twcomponent.h"
#include "twrandomsource.h"
#include "twsamplereader.h"
#include "twgrainsource.h"
#include "sapplication.h"
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
    if( !rs ) return;   // non-source content: getRootComponent() falls back

    twRandomSource *view = rs;
    if( !grainParams_.isIdentity() ) {
        grain_ = new twGrainSource( *rs, grainParams_ );
        view = grain_;
    }
    reader_ = view->acquireReader( *(SApplication::app().get303aEnvironment()) );
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
    if( reader_ ) return reader_->seekTo( off + startOffset_ );
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
    delete content_;
    content_ = NULL;
}

SCut::SCut( SProject *parentProject, SLink &content )
    : SObject( parentProject ),
      startOffset_( 0 ),
      inlineRenderer_( NULL ),
      reader_( NULL ),
      grain_( NULL ),
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
      inlineRenderer_( NULL ),
      reader_( NULL ),
      grain_( NULL ),
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

    // Grain params are stored already-final, so set them directly (no rescale).
    grainParams_.stretch    = element.attribute( "stretch", "1.0" ).toDouble();
    grainParams_.pitchCents = element.attribute( "pitchCents", "0.0" ).toDouble();
    grainParams_.grainSize  = element.attribute( "grainSize", "2048" ).toLongLong();
    grainParams_.crossfade  = element.attribute( "crossfade", "512" ).toLongLong();

    // Pre-build the grain buffer now (load thread) so a stretched clip restored
    // from disk does not materialise on the first realtime audio block.
    if( !grainParams_.isIdentity() ) rebuildReader();

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
