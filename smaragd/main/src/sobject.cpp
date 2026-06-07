
#include <stdlib.h>
#include <math.h>
#include <cstdint>

#include <qobject.h>
#include <qtextstream.h>
#include <QChildEvent>

#include "sobject.h"
#include "slink.h"
#include "sproject.h"

void SObject::setSolo( bool f )
{
    if( f==solo_ ) return;
    solo_ = f;
    emit soloChanged( f );
}

void SObject::setMuted( bool f )
{
    if( f==muted_ ) return;
    muted_ = f;
    emit mutedChanged( f );
}

void SObject::setVolume( double d )
{
    if( fabs( volume_-d ) < 0.0001 ) return;
    volume_ = d;
    emit volumeChanged( d );
}

void SObject::setPan( double d )
{
    if( d<-1.0 ) d = -1.0;
    else if( d>1.0 ) d= 1.0;
    if( fabs( pan_-d ) < 0.00001 ) return;
    pan_ = d;
    emit panChanged( d );
}

void SObject::setDelay( double d )
{
    if( fabs( delay_-d ) < 0.000001 ) return;
    delay_ = d;
    emit delayChanged( d );
}

void SObject::setSName( const QString &n )
{
    QString newName;
    if( n=="" ) newName = "(untitled)";
    if( newName==sName_ ) return;
    sName_ = newName;
    emit sNameChanged( newName );
}

int SObject::serializeSelfAttributes( QTextStream &o )
{
    o << " id='" << reinterpret_cast<std::uintptr_t>(this) << "'"
      << " nRefs='" << nRefs_ << "'"
      << " hasDuration='" << hasDuration() << "'";
    if( hasDuration() ) {
        o << " duration='" << (double)getDuration() << "'";
    }
    o << " muted='";
    o << (isMuted()?"true":"false") << "'";
    o << " solo='";
    o << (isSolo()?"true":"false") << "'";
    o << " volume='" << getVolume() << "'";
    o << " pan='" << getPan() << "'";
    o << " delay='" << getDelay() << "'";
    return 0;
}

int SObject::readPreChildrenAttributes( QDomElement &element )
{
    QString data;
    data = element.attribute( "duration", "10000" );
    setDuration( data.toULongLong() );
    data = element.attribute( "muted", "false" );
    setMuted( data.startsWith( "true" ) );
    data = element.attribute( "solo", "false" );
    setSolo( data.startsWith( "true" ) );
    data = element.attribute( "volume", "1.0" );
    setVolume( data.toDouble() );
    data = element.attribute( "pan", "0.0" );
    setPan( data.toDouble() );
    data = element.attribute( "delay", "0.0" );
    setDelay( data.toDouble() );
    return 0;
}

int SObject::readPostChildrenAttributes( QDomElement &element )
{
    (void) element;
    return 0;
}

int SObject::serialize( QTextStream &o )
{
    int res;
    o << "<" << metaObject()->className();
    res = serializeSelfAttributes( o );
    if( res<0 ) return res;
    o  << ">\n";

    for( SLink *lk : childLinks() ) {
        int res = lk->serialize( o );
        if( res<0 ) break;
    }

    o << "</" << metaObject()->className() << ">\n";
    return 0;
}

void SObject::invalidatePreview()
{
    if( !previewData_ ) return;
    ::free( previewData_ );
    previewData_ = NULL;    
}

int SObject::getPreview( preview_t */*dest*/,
                         offset_t /*start*/, length_t /*length*/, 
                         offset_t /*nProbes*/ )
{
    return -1;
}

int SObject::straightCalcPreviewData()
{
    if( previewData_ ) return 0;
    if( !hasDuration() ) return -1;    
    previewSkip_ = 256;
    previewForLength_ = getDuration();
    if( !previewForLength_ ) return -2;
    // Create adequate resolution.
    while( previewForLength_<(previewSkip_*128)
           && previewSkip_>0 ) {
        previewSkip_ >>= 1;
    }
    if( !previewSkip_ ) previewSkip_ = 1;
    while( true ) {
        nPreviewProbes_ = previewForLength_ / previewSkip_;
        if( nPreviewProbes_ < 0x200000 ) break;
        previewSkip_ *= 2;        
    }
    // FOr the last, incomplete one.
    nPreviewProbes_++;
    previewData_ = (preview_t *) ::calloc( sizeof( preview_t ), nPreviewProbes_ );
    if( !previewData_ ) return -3;
    qWarning( "SObject::straightCalcPreviewData(): Allocating %d*%d bytes of preview data, "
              "nPreviewProbes_ = %d, previewSkip_ = %d.\n",
              (int)sizeof( preview_t ), (int)nPreviewProbes_,
              (int)nPreviewProbes_, (int)previewSkip_ );
    sample_t *buffer = (sample_t *) alloca( previewSkip_ * sizeof( sample_t ) );
    // Fill it up.
    for( offset_t i=0; i<(offset_t) previewForLength_; i+=previewSkip_ ) {
	// Yes, this values are other way round. These are the defaults, 
	// we want to calc the overall range.
        sample_t min=SAMPLE_NORM_MAX, max = SAMPLE_NORM_MIN;
        getRootComponent().seekTo( i );
        getRootComponent().calcOutputTo( buffer, previewSkip_, 0 );
        sample_t *p = buffer;
        for( offset_t j=0; j<previewSkip_; ++j ) {
            sample_t a = *p++;
            if( a<min ) min = a;
            if( a>max ) max = a;
        }
	// Now clip to signed 8 bit.
        max = (max*127.) / SAMPLE_NORM_MAX;
        if( max>127. ) max=127.;
        if( max<-128. ) max=-128.;
        min = (min * -127.) / SAMPLE_NORM_MIN;
        if( min>127.) min = 127.;
        if( min<-128.) min = -128.;
        int idx = i/previewSkip_;
        if( idx>=(int)nPreviewProbes_ ) {
            qWarning( "Straight preview store out of range.\n" );
        } else {
            previewData_[i/previewSkip_].min = (char) min;
            previewData_[i/previewSkip_].max = (char) max;
        }
    }
    return 0;
}

int SObject::getStraightPreview( preview_t *dest,
                                 offset_t start, length_t length, 
                                 offset_t nProbes )
{
    int res;
    length_t myLen;
    if( !hasDuration() ) return -1;
    myLen = getDuration();
    if( !myLen ) return -3;
    if( !previewData_ ) {
        res = straightCalcPreviewData();
        if( res<0 ) {
            qWarning( "Error calculating preview data.\n" );
            return res;
        }
    }
    if( !previewData_ ) {
        qWarning( "Error calculating preview data although clamied he had.\n" );
        return -4;
    }
    // FIXME: Check start and length.
    if( length < 1 ) length = 1;
    for( offset_t i=0; i<nProbes; i++ ) {
        // FIXME: Overflows??? Doubles??
        offset_t realPos = start + ((i*length) / nProbes);
        offset_t probeIdx = realPos/previewSkip_;
        preview_t v1 = previewData_[probeIdx];
        *dest++ = v1;
    }
    return 0;
}

bool SObject::hasPreview() const
{
    return false;
}

void SObject::setDuration( length_t )
{
    // FIXME: ENOSYS.
}

int SObject::seekTo( offset_t ofs )
{
    return getRootComponent().seekTo( ofs );
}

int SObject::getNReferences() const
{
    return nRefs_;
}

SLink *SObject::childAt( int index ) const
{
    if( index<0 || index>=childOrder_.size() ) return nullptr;
    return childOrder_.at( index );
}

int SObject::indexOfChild( const SLink *child ) const
{
    return childOrder_.indexOf( const_cast<SLink*>( child ) );
}

int SObject::indexOfChildObject( const SObject &child ) const
{
    for( int i=0; i<childOrder_.size(); ++i ) {
        if( &childOrder_.at( i )->getSObject() == &child ) return i;
    }
    return -1;
}

void SObject::moveChildToIndex( int fromIndex, int toIndex )
{
    const int n = childOrder_.size();
    if( fromIndex<0 || fromIndex>=n ) return;
    if( toIndex<0 ) toIndex = 0;
    if( toIndex>=n ) toIndex = n-1;
    if( fromIndex==toIndex ) return;
    // Order is just the explicit list; QObject parentage is unaffected.
    childOrder_.move( fromIndex, toIndex );
}

void SObject::addRef()
{
    if( ++nRefs_ == 1 ) {
        emit gotReferenced();
    }
    qWarning( "Object of class \"%s\" now has %d references.\n", 
              metaObject()->className(), nRefs_ );
    emit nRefsChanged();
}

void SObject::removeRef()
{
    if( nRefs_==0 ) {
        qWarning( "SObject::removeRef(): Called although reference count was zero.\n" );
        return;
    }
    if( (--nRefs_)==0 ) {
        qWarning( "Object %p, class '%s' got unreferenced.\n",
                  (void *)this, metaObject()->className() );
        emit gotUnreferenced();
    }
    qWarning( "Object of class \"%s\" now has %d references.\n", 
              metaObject()->className(), nRefs_ );
    emit nRefsChanged();
    if( 0==nRefs_ ) {
        // This will delete the object if the application reenters the main loop.
        deleteLater();
    }
}

/**
 * This is a simple method to scan through all children.
 * Every link has a start time, every object (maybe) a duration.
 * We assume, that all of my children belong to my events.
 */
offset_t SObject::getChildrenExtent( offset_t &firstStart, offset_t &lastEnd, 
                                     int &nUndefStart, int &nUndefDuration ) const
{    
    nUndefStart = 0;
    nUndefDuration = 0;
    firstStart = (offset_t) (0-1); // Largest number possible
    lastEnd = (offset_t) 0;
    for( SLink *sli : childLinks() ) {
        SObject &sobj = sli->getSObject();
        bool hd = sobj.hasDuration();

        if( sli->hasStartTime() ) {
            offset_t st = sli->getStartTime();
            if( st<firstStart ) firstStart = st;
            if( hd ) {
                offset_t du = (offset_t) sobj.getDuration();
		// qWarning( "SObject::getChildrenExtent(): Duration = %d:%d.",
		//	  (int)(du>>32),(int)du );
                du += st;
		// qWarning( "SObject::getChildrenExtent(): Duration = %d:%d.",
		//	  (int)(du>>32),(int)du );
                if( du>lastEnd ) lastEnd = du;
            }
        } else {
            ++nUndefStart;
        }
        if( !hd ) {
            ++nUndefDuration;
        }
    } 
    return lastEnd-firstStart;
}

bool SObject::hasDuration() const
{
    offset_t first, last;
    int nUndefStart, nUndefDuration;
    getChildrenExtent( first, last, nUndefStart, nUndefDuration );
    return nUndefStart==0 && nUndefDuration==0;
}

offset_t SObject::getFirstChildStartTime() const
{
    offset_t first, last;
    int nUndefStart, nUndefDuration;
    getChildrenExtent( first, last, nUndefStart, nUndefDuration );
    return first;
}    

length_t SObject::getDuration() const
{
    return getAllChildsDuration();
}    

length_t SObject::getAllChildsDuration() const
{
    offset_t first, last;
    int nUndefStart, nUndefDuration;
    getChildrenExtent( first, last, nUndefStart, nUndefDuration );
    return last-first;
}    

bool SObject::isEmpty() const
{
    return childOrder_.isEmpty();
}

void SObject::childEvent( QChildEvent *ce )
{
    QObject::childEvent( ce );
    // Keep childOrder_ membership in sync with QObject parentage. New children
    // append (order is then adjusted via moveChildToIndex); removed children
    // drop out. Mirror QObject's own list state: at ChildAdded the child is
    // already in children(); at ChildRemoved it is already gone.
    if( ce->added() ) {
        SLink *lk = (SLink *) ce->child();
        if( !childOrder_.contains( lk ) ) childOrder_.append( lk );
        gotChild( *lk );
    } else if( ce->removed() ) {
        SLink *lk = (SLink *) ce->child();
        childOrder_.removeOne( lk );
        lostChild( *lk );
    }
}

void SObject::gotChild( SLink &newChild )
{
    emit childObjectAdded( newChild );
}

void SObject::lostChild( SLink &newChild )
{
    emit childObjectRemoved( newChild );
}

int SObject::getChildIndex( SObject &child ) const
{
    return indexOfChildObject( child );

}

SObject::SObject( SProject *project )
    : QObject( project ),
      nRefs_( 0 ),
      previewForLength_( 0 ),
      nPreviewProbes_( 0 ),
      previewData_( 0 ),   
      previewSkip_( 0 ),
      solo_( false ),
      muted_( false ),
      volume_( 0.0 ),
      pan_( 0.0 ),
      delay_( 0.0 ),
      sName_( "(untitled)" )
{
    // We neither want to remember  previews if we have changed our duration
    // (Although we could reimplement it for that special case)
    // nor for the being unreferenced (just wastes memory).
    QObject::connect( this, SIGNAL( durationChanged( length_t ) ),
                      this, SLOT( invalidatePreview() ) );
    QObject::connect( this, SIGNAL( gotUnreferenced() ),
                      this, SLOT( invalidatePreview() ) );
}

SObject::~SObject()
{
}
