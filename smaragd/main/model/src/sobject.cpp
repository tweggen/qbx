
#include <stdlib.h>
#include <math.h>
#include <cstdint>

#include <qobject.h>
#include <qtextstream.h>
#include <QChildEvent>

#include "app/model/sobject.h"
#include "tw/schedule/capture_aspects.h"  // Preview/Playback/Metadata/Export bits
#include "app/model/slink.h"
#include "app/model/sproject.h"
#include "tw/sources/twrandomsource.h"
#include "tw/schedule/capture_revalidator.h"

void SObject::setSolo( bool f )
{
    if( f==solo_ ) return;
    solo_ = f;
    emit soloChanged( f );
    // Solo changes both preview rendering (affects visibility in composites)
    // and audio routing. Invalidate all three aspects.
    notifyDependentsChanged(Preview | Playback | Metadata);
}

void SObject::setMuted( bool f )
{
    if( f==muted_ ) return;
    muted_ = f;
    emit mutedChanged( f );
    // Mute changes both preview rendering (affects visibility in composites)
    // and audio routing. Invalidate all three aspects.
    notifyDependentsChanged(Preview | Playback | Metadata);
}

void SObject::setArmedForRecording( bool f )
{
    if( f==armed_ ) return;
    armed_ = f;
    emit armedForRecordingChanged( f );
    // Armed state doesn't affect audio capture aspects (only matters during record),
    // but invalidate for UI consistency
    notifyDependentsChanged(Metadata);
}

void SObject::setRecordingChannels( uint32_t channels )
{
    if( channels == recordingChannels_ ) return;
    recordingChannels_ = channels;
    emit recordingChannelsChanged( channels );
}

void SObject::setVolume( double d )
{
    {
        std::lock_guard<std::mutex> lock( volumeMutex_ );
        if( fabs( volume_-d ) < 0.0001 ) return;
        volume_ = d;
    }
    // Volume change affects preview rendering, so invalidate the cached preview
    // so it gets regenerated at the new volume level (not just scaled on-the-fly).
    invalidatePreview();
    emit volumeChanged( d );
    // Volume changes audio content but not arrangement:
    // notify dependents to invalidate Playback + Metadata aspects (audio content change)
    notifyDependentsChanged(Playback | Metadata);
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
        // Store duration as time (seconds) for rate independence
        int srate = parent() && parent()->parent() ?
            dynamic_cast<SProject*>(parent()->parent())->getSRate() : 48000;
        double durationSec = getDuration() / (double)srate;
        o << " durationSec='" << durationSec << "'";
    }
    o << " muted='";
    o << (isMuted()?"true":"false") << "'";
    o << " solo='";
    o << (isSolo()?"true":"false") << "'";
    o << " armedForRecording='";
    o << (isArmedForRecording()?"true":"false") << "'";
    o << " volume='" << getVolume() << "'";
    o << " pan='" << getPan() << "'";
    o << " delay='" << getDelay() << "'";
    return 0;
}

int SObject::readPreChildrenAttributes( QDomElement &element )
{
    QString data;

    // Load duration: try new time-based format first (durationSec), then
    // fall back to old sample-based format for backwards compatibility
    int srate = parent() && parent()->parent() ?
        dynamic_cast<SProject*>(parent()->parent())->getSRate() : 48000;

    QString durationSecStr = element.attribute( "durationSec" );
    if( !durationSecStr.isEmpty() ) {
        // New format: stored as seconds, convert to samples
        double durationSec = durationSecStr.toDouble();
        setDuration( (length_t)( durationSec * srate + 0.5 ) );
    } else {
        // Backwards compatibility: old format was samples, default 10000 @ 48kHz
        data = element.attribute( "duration" );
        if( data.isEmpty() ) {
            // Default: ~208 ms (10000 samples @ 48kHz)
            setDuration( (length_t)( 0.208333 * srate + 0.5 ) );
        } else {
            length_t oldSamples = data.toULongLong();
            // Assume old value was at 48kHz, scale proportionally
            setDuration( (length_t)( ( oldSamples * srate ) / 48000.0 + 0.5 ) );
        }
    }
    data = element.attribute( "muted", "false" );
    setMuted( data.startsWith( "true" ) );
    data = element.attribute( "solo", "false" );
    setSolo( data.startsWith( "true" ) );
    data = element.attribute( "armedForRecording", "false" );
    setArmedForRecording( data.startsWith( "true" ) );
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

int SObject::getPreview( preview_t *dest,
                         offset_t start, length_t length,
                         offset_t nProbes )
{
    // Default preview: if this object renders to a duration, produce peaks from
    // its rendered output. straightCalcPreviewData() reads a random source when
    // there is one (samples) and otherwise pulls getRootComponent() live — so a
    // container (a track/mixer sub-arrangement, i.e. a live asset) is previewable
    // too. No duration -> no preview.
    if( !hasDuration() ) return -1;
    return getStraightPreview( dest, start, length, nProbes );
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
    // If this object exposes random-access sample data, read it statelessly:
    // that touches no play cursor and needs no lock, so preview rendering on the
    // UI thread can no longer race playback on the audio thread (proposal 07).
    twRandomSource *rs = getRandomSource();
    // Fill it up.
    for( offset_t i=0; i<(offset_t) previewForLength_; i+=previewSkip_ ) {
	// Yes, this values are other way round. These are the defaults,
	// we want to calc the overall range.
        sample_t min=SAMPLE_NORM_MAX, max = SAMPLE_NORM_MIN;
        if( rs ) {
            rs->read( i, buffer, previewSkip_, 0 );
        } else {
            getRootComponent().seekTo( i );
            getRootComponent().calcOutputTo( buffer, previewSkip_, 0 );
        }
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
        // Clamp probeIdx to valid range [0, nPreviewProbes_-1] to prevent out-of-bounds access
        if( probeIdx >= nPreviewProbes_ ) {
            probeIdx = nPreviewProbes_ - 1;
        }
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
    emit nRefsChanged();
}

void SObject::removeRef()
{
    if( nRefs_==0 ) {
        qWarning( "SObject::removeRef(): Called although reference count was zero.\n" );
        return;
    }
    if( (--nRefs_)==0 ) {
        emit gotUnreferenced();
    }
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

    const auto& links = childLinks();
    if( links.isEmpty() ) {
        return 0;
    }

    for( SLink *sli : links ) {
        if( !sli ) {
            ++nUndefStart;
            ++nUndefDuration;
            continue;
        }

        // Safely get the child object; skip if invalid
        SObject *sobj_ptr = nullptr;
        try {
            sobj_ptr = &sli->getSObject();
        } catch (...) {
            ++nUndefStart;
            ++nUndefDuration;
            continue;
        }

        if( !sobj_ptr ) {
            ++nUndefStart;
            ++nUndefDuration;
            continue;
        }

        bool hd = false;
        try {
            hd = sobj_ptr->hasDuration();
        } catch (...) {
            hd = false;
        }

        if( sli->hasStartTime() ) {
            offset_t st = sli->getStartTime();
            if( st<firstStart ) firstStart = st;
            if( hd ) {
                try {
                    offset_t du = (offset_t) sobj_ptr->getDuration();
		    // qWarning( "SObject::getChildrenExtent(): Duration = %d:%d.",
		    //	  (int)(du>>32),(int)du );
                    du += st;
		    // qWarning( "SObject::getChildrenExtent(): Duration = %d:%d.",
		    //	  (int)(du>>32),(int)du );
                    if( du>lastEnd ) lastEnd = du;
                } catch (...) {
                    // Ignore errors in getting duration
                }
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
      armed_( false ),
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

SProject *SObject::getProjectSafe() const
{
    return dynamic_cast<SProject*>( parent() );
}

// Register a dependent link (object that references this one).
// Called when a cut or asset placement references this object.
// Uses SLink (the native way to track references) for dependency tracking.
// Connects to the link's destroyed signal to auto-unregister if the link is deleted.
void SObject::addDependentLink(SLink *dependentLink)
{
    if (!dependentLink) return;
    {
        std::lock_guard<std::mutex> lock(dependentsMutex_);
        dependentLinks_.insert(dependentLink);
    }

    // Auto-unregister this link when it's destroyed (proposal 06: safe cleanup).
    // If the cut/link is deleted, remove it from our dependent set to avoid
    // stale pointers. Use a lambda to capture 'this' and the link safely.
    QObject::connect(dependentLink, &QObject::destroyed,
                     this, [this, dependentLink]() {
        removeDependentLink(dependentLink);
    });
}

// Unregister a dependent link. Called when a reference is removed.
void SObject::removeDependentLink(SLink *dependentLink)
{
    if (!dependentLink) return;
    std::lock_guard<std::mutex> lock(dependentsMutex_);
    dependentLinks_.remove(dependentLink);
}

// Notify dependent links that specific aspects have changed.
// Only affected dependents are invalidated (lazy invalidation).
// Example: setMuted() → notifyDependentsChanged(Playback | Metadata)
//
// This allows fine-grained invalidation: muting a track only invalidates
// the Playback aspects of cuts that capture that track's output, not the
// entire arrangement or unrelated clips.
//
// NOTE: Links are auto-unregistered on destruction via the destroyed() signal
// connected in addDependentLink(). This is safe even if a link is destroyed
// while we're iterating (we collect links under lock first).
void SObject::notifyDependentsChanged(uint32_t affectedAspects)
{
    // Collect dependents under lock, then notify without holding lock.
    // This snapshot approach is safe: even if a link is destroyed during iteration,
    // the destroyed() signal will trigger removeDependentLink(), but we're working
    // on our own snapshot, not the shared set.
    QSet<SLink*> dependents;
    {
        std::lock_guard<std::mutex> lock(dependentsMutex_);
        dependents = dependentLinks_;
    }

    // Notify each dependent (typically a cut) to invalidate affected aspects.
    // Virtual dispatch: the base no-op covers objects without captures, so
    // this file needs no knowledge of concrete types.
    for (SLink *link : dependents) {
        if (!link) continue;  // Defensive: skip if somehow null (shouldn't happen)
        link->getSObject().invalidateAspects(affectedAspects);
    }
}

// Phase 5e: Page cache API
// Base implementation: just returns current page without scheduling.
// Derived classes (SCut, STrack, etc.) override to call scheduleRevalidation().
double SObject::volumeDbSnapshot() const
{
    std::lock_guard<std::mutex> lock( volumeMutex_ );
    return getVolume();
}

std::shared_ptr<CapturePageData> SObject::getCapture(uint32_t aspectsMask)
{
    if (aspectsMask == 0) return nullptr;

    // Get current page (may be stale, may be null, never blocks).
    // Read current page without locking (shared_ptr copy is atomic).
    auto page = currentPage();

    // If current page has all needed aspects, return immediately.
    // Acquire page lock to safely read validAspects (prevents torn reads during concurrent writes).
    if (page) {
        std::lock_guard<std::mutex> pageLock(page->pageMutex);
        if ((page->validAspects & aspectsMask) == aspectsMask) {
            return page;
        }
    }

    // TODO: Phase 5e.5 - Unify CaptureRevalidator to work with SObject*.
    // For now, derived classes (SCut) override this to call scheduleRevalidation().

    // Return current page anyway (stale is OK; better than null/dropout)
    return page;
}

bool SObject::needsRevalidation_nolock(uint32_t aspectsMask) const
{
    if (aspectsMask == 0) return false;
    // Note: _nolock refers to not holding SObject's state mutex; we still need page lock for valid aspect check
    if (!currentPage_) return true;
    std::lock_guard<std::mutex> pageLock(currentPage_->pageMutex);
    return (currentPage_->validAspects & aspectsMask) != aspectsMask;
}
