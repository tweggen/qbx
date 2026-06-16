
#include <iostream>
#include <stdlib.h>

#include <QDebug>

#include "twrewire.h"
#include "twmixer.h"

#include "sstdmixer.h"
#include "sstdmixerview.h"
#include "sprojectloader.h"
#include "sapplication.h"

using namespace std;

/**
 * We define a track's duration as the ending point of the last terminated event,
 * otherwise 1.
 */
bool SStdMixer::hasDuration() const
{
    return true;
}

/**
 * We define a track's duration as the ending point of the last terminated event,
 * otherwise 1.
 */
length_t SStdMixer::getDuration() const
{
    if( !lastDurationValid_ ) {
        offset_t first, last=0;
        int nUndefStart, nUndefDuration;
        getChildrenExtent( first, last, nUndefStart, nUndefDuration );
        if( last>0 ) { 
            lastDuration_ = last;
        } else {
            lastDuration_ = 1;
        }
        lastDurationValid_ = true;
    }
    return lastDuration_;    
}

QWidget *SStdMixer::getDetailEditWidget( QWidget *parent )
{
    return new SStdMixerView( parent, this );
}

QWidget *SStdMixer::getInlineEditWidget( QWidget * )
{
    return NULL;
}

SObjectRenderer *SStdMixer::getInlineRenderer()
{
    return NULL;
}

int SStdMixer::getNTracks() const
{
    return childCount();
}

#if 0
SSMPerTrack *SStdMixer::getPerTrackAt( int idx )
{
    SSMPerTrack *spt = trackList_.at( idx );
    if( !spt ) return NULL;    
    return spt;
}
#endif

SLink *SStdMixer::getTrackAt( int idx )
{
    return childAt( idx );
}

#if 0
int SStdMixer::getTrackIndex( STrack &trk )
{
    QPtrListIterator<SSMPerTrack> it( trackList_ );
    int idx = 0;
    SSMPerTrack *spt;
    while ( (spt=it.current()) != 0 ) { 
        ++it;
        if( spt &&
            ((SObject *)&trk)
            ==(SObject *)&(spt->getTrack().getSObject()) ) return idx;
        ++idx;
    }
    return -1;
}
#endif

#if 0
SSMPerTrack *SStdMixer::findTrack( STrack &trk )
{
    QPtrListIterator<SSMPerTrack> it( trackList_ );
    int idx = 0;
    SSMPerTrack *spt;
    while ( (spt=it.current()) != 0 ) { 
        ++it;
        if( spt
            && ((SObject *)&trk)
            ==((SObject *)&(spt->getTrack().getSObject())) ) return spt;
        ++idx;
    }
    return NULL;
}
#endif

twComponent &SStdMixer::getRootComponent()
{
    // FIXME: Generate a channel reassignment.
    return *cpRewire_;
}

int SStdMixer::seekTo( offset_t off )
{
    for( SLink *lk : childLinks() ) {
        lk->getSObject().seekTo( off );
    }
    return 0;
}

/**
 * Remove all inputs from each of the mixers.
 * Add all track outputs again to each of the bus mixers.
 */
void SStdMixer::reconnectTracksToMixer()
{
    if( !cpMixers_ ) {
        qWarning( "Should reconnect tracks although no mixer was created yet.\n" );
        return;
    }
    int nTracks = childCount();
    const bool solo = anyTrackSoloed();
    // For all busses.
    for( int bus=0; bus<nBusses_; bus++ ) {
        twMixer *mix = cpMixers_[bus];
        if( !mix ) continue;
        // Ensure the given number of inputs.
        mix->setNInputs( nTracks );
        for( int channel=0; channel<nTracks; channel++ ) {
            SLink *lk = childAt( channel );
            // A track is audible iff it is not muted and, when any track is
            // soloed, it is itself soloed. Inaudible tracks get a NULL input so
            // their DSP is not pulled at all (processing AND output disabled).
            bool audible = false;
            if( lk ) {
                SObject &so = lk->getSObject();
                audible = !so.isMuted() && ( !solo || so.isSolo() );
            }
            if( !lk || !audible ) {
                mix->setInput( channel, NULL );
                mix->setInputLevel( channel, 0 );
            } else {
                twComponent &root = lk->getRootComponent();
                mix->setInput( channel, root.linkOutput( bus ) );
                // Unity (0 dB): the track applies its own gain intrinsically
                // (see twTrackMix::calcOutputTo), so the mixer just sums.
                mix->setInputLevel( channel, 0.0 );
            }
        }
    }
}

/**
 * True if at least one track has its solo flag set.
 */
bool SStdMixer::anyTrackSoloed() const
{
    for( SLink *lk : childLinks() ) {
        if( lk->getSObject().isSolo() ) return true;
    }
    return false;
}

/**
 * A track's mute/solo changed. Solo is global (it silences every non-soloed
 * track), so just re-evaluate the whole routing.
 */
void SStdMixer::trackMuteSoloChanged()
{
    reconnectTracksToMixer();
}

// Slots.

/**
 * Correct the mixer assignments on track removal.
 * 
 * OK, brute force for now: Delete the old track outputs, reconnect
 * them to the mixer inputs.
 */
void SStdMixer::mixerUpdateTrackRemoved( int, STrack & )
{
    reconnectTracksToMixer();
    lastDurationValid_ = false;
    checkDurationChanged();
}

void SStdMixer::mixerUpdateTrackAdded( int, STrack & )
{
    reconnectTracksToMixer();
    lastDurationValid_ = false;
    checkDurationChanged();
}

/**
 * Set the number of busses for this mixer.
 * If any objects are connected to bus the would vanish,
 * an error occurs.
 */
int SStdMixer::setNBusses( int n )
{
    twMixer **newMixers, **oldMixers = cpMixers_;
    // no change?
    if( n<0 ) return -2;
    if( n==nBusses_ && cpMixers_ ) return 0;
    int nTracks = childCount();

    qWarning( "SStdMixer::setNBusses( %d ): called.\n", n );

    // First check, if still input tracks are connected to the
    // busses that should vanish.
    if( n<nBusses_ && oldMixers ) {
        for( int i=n; i<nBusses_; i++ ) {
            int still = oldMixers[i]->getInputsSet();
            if( still ) {
                qWarning( "SStdMixer::setNBusses(): Unable to remove channel %d: "
                          "Still %d objects connected.\n", i, still );
                return -1;
            }
        }
    }
    // OK, we might shrink or enlarge. Take the output latches we have with us.
    newMixers = (twMixer **) ::calloc( sizeof( twMixer * ), n );
    // Takeover contains the the number of busses we can takeover from the current installation.
    int nTakeOver = n;
    if( nBusses_<nTakeOver ) nTakeOver = nBusses_;
    // Use the old mixers
    for( int i=0; i<nTakeOver; i++ ) {
        qWarning( "SStdMixer::setNBusses( %d ): Using old mixer #%d.\n", n, i );
        newMixers[i] = cpMixers_[i];
    }
    // If we have to free old unused ones, do it.
    // In the same shot, we remove them from our rewirer.
    for( int i=nTakeOver; i<nBusses_; i++ ) {
        cpRewire_->setInput( i, NULL );
        qWarning( "SStdMixer::setNBusses( %d ): Deleting old mixer #%d.\n", n, i );
        delete cpMixers_[i];
        cpMixers_[i] = NULL;
    }
    // Set the rewirer to the proper number of channels.
    cpRewire_->setNPlugs( n );

    // If we have to alloc new ones, do it.
    for( int i=nTakeOver; i<n; i++ ) {
        qWarning( "SStdMixer::setNBusses( %d ): Creating new mixer #%d.\n", n, i );
        int nc = nTracks;
        if( nc<1 ) nc = 1;
        twMixer *mix = new twMixer( *(SApplication::app().get303aEnvironment()), nc );
        mix->init();
        cpRewire_->setInput( i, mix->linkOutput( 0 ) );
        newMixers[i] = mix;
    }
    nBusses_ = n;
    cpMixers_ = newMixers;
    if( oldMixers ) ::free( oldMixers );

    // Wire every existing track into every freshly-created bus mixer.
    // reconnectTracksToMixer iterates over all (bus, track) pairs and
    // calls setInput / setInputLevel correctly — replaces the previous
    // misindexed priming which used the bus index to look up a child
    // track and set the bus mixer's like-indexed input level. The
    // per-track volumeChanged signal connection is handled in
    // insertTrack(), not here.
    reconnectTracksToMixer();

    return 0;
}

void SStdMixer::insertTrack( STrack &trk )
{
    QObject::connect( (QObject*)&trk, SIGNAL( durationChanged( length_t ) ),
                      this, SLOT( mixerChildDurationChanged( length_t ) ) );
    // Volume needs no connection: twTrackMix reads getVolume() live each buffer.
    QObject::connect(
        (QObject*)&trk, SIGNAL( mutedChanged( bool ) ),
        this, SLOT( trackMuteSoloChanged() ) );
    QObject::connect(
        (QObject*)&trk, SIGNAL( soloChanged( bool ) ),
        this, SLOT( trackMuteSoloChanged() ) );
    // Construction parents the link to us, which appends it (childEvent keeps
    // childOrder_ in sync). Position it afterwards with reorderTrack().
    SLink *lk = new SLink( (SObject&)trk, this );
    (void) lk;
    int newIndex = childCount() - 1;   // actual landing index (append)
    qWarning( "Inserted new track @%d.\n", newIndex );
    emit trackInserted( newIndex, trk );
}

void SStdMixer::notifyTreeChanged()
{
    emit tracksReordered();
}

void SStdMixer::reorderTrack( int fromIndex, int toIndex )
{
    const int n = childCount();
    if( fromIndex<0 || fromIndex>=n ) return;
    if( toIndex<0 ) toIndex = 0;
    if( toIndex>=n ) toIndex = n-1;
    if( fromIndex==toIndex ) return;
    moveChildToIndex( fromIndex, toIndex );
    // Bus mixer inputs are assigned by track index, so re-wire after a reorder.
    reconnectTracksToMixer();
    emit tracksReordered();
}

int SStdMixer::removeTrack( SLink &track )
{
    int idx = getChildIndex( track.getSObject() );
    SLink *sl = childAt( idx );
    if( !sl ) return -1;
    QObject::disconnect( &(sl->getSObject()), SIGNAL( durationChanged( length_t ) ), 
                         this, SLOT( mixerChildDurationChanged( length_t ) ) );    
    QObject::disconnect( &(sl->getSObject()), SIGNAL( volumeChanged( double ) ), 
                         this, SLOT( trackVolumeChanged( double ) ) );
    return removeTrack( idx );
}

int SStdMixer::removeTrack( int trackIndex )
{
    SLink *stl = childAt( trackIndex );
    if( !stl ) {
        qWarning( "SStdMixer::removeTrack(): Child requested was not found.\n" );
        return -1;
    }
    SObject &so = stl->getSObject();
    // Delete the link FIRST so that getNTracks()/children reflect the removal
    // before listeners react. The track object itself survives (its refcount
    // drops to 0 and it is deleteLater'd), so passing it to the signal is safe;
    // reconnectTracksToMixer() then rewires only the remaining tracks instead of
    // leaving a dangling input to the removed one.
    delete stl;
    emit trackRemoved( trackIndex, (STrack &)so );
    return 0;
}

void SStdMixer::checkDurationChanged()
{
    length_t oldDuration = lastDuration_;
    length_t newDuration = getDuration();
    qWarning( "SStdMixer::checkDurationChanged() called. oldDuration = %d, newDuration = %d.",
              (int)oldDuration, (int)newDuration );
    qWarning( "SStdMixer::checkDurationChanged(): newDuration = %d:%d.",
	      (int)(newDuration>>32), (int)newDuration );
    if( oldDuration!=newDuration ) {
        emit durationChanged( newDuration );
    }
}

void SStdMixer::mixerChildDurationChanged( length_t )
{
    lastDurationValid_ = false;
    checkDurationChanged();
}

SStdMixer::~SStdMixer()
{
    // FIXME: Free cpMixer.
}

SStdMixer::SStdMixer( SProject *project )
    : SObject( project ),
      cpMixers_( NULL ),
      cpRewire_( NULL ),
      nBusses_( 0 ),
      lastDuration_( 1 ),
      lastDurationValid_( true )
{
    cpRewire_ = new twRewire( *(SApplication::app().get303aEnvironment()) );
    cpRewire_->init();
    QObject::connect( this, SIGNAL( trackInserted( int, STrack & ) ),
                      this, SLOT( mixerUpdateTrackAdded( int, STrack & ) ) );
    QObject::connect( this, SIGNAL( trackRemoved( int, STrack & ) ),
                      this, SLOT( mixerUpdateTrackRemoved( int, STrack & ) ) );
    // Start with one bus so reconnectTracksToMixer() (called when a track
    // is added) has somewhere to wire the track into. The original code
    // called setNBusses(0) here and relied on the .qxp loader to set a
    // real bus count later, which left File → New projects silent — no
    // bus mixer existed, so reconnectTracksToMixer's outer loop did
    // nothing and tracks were never wired.
    setNBusses( 1 );
}

void SStdMixer::setSelectedTrack( STrack *track )
{
    if( selectedTrack_ != track ) {
        selectedTrack_ = track;
        emit selectedTrackChanged( track );
    }
}

SLink *SStdMixer::instantiateFromDomElement(
    SProjectLoader &projectLoader, QDomElement &element, SObject *parent )
{
    SLink *contentLink = NULL;
    // Find the first link child 
    QDomNode childNode = element.firstChild();
    SStdMixer *mixer = new SStdMixer( &projectLoader.getProject() );
    while( !childNode.isNull() ) {
        if( childNode.isElement() ) {
            qWarning() << "found SStdMixer child " << childNode.nodeName() << Qt::endl;
            if( childNode.nodeName() == "SLink" ) {
                QDomElement childElement = childNode.toElement();
                QString objectId = childElement.attribute( "objectId" );
                // Look up the object id.
                contentLink = projectLoader.getObjectDictionary().value( objectId );
                if( contentLink ) {
                    // FIXME: Check, wether this is a track, or create a generic insertion function.
                    mixer->insertTrack( *(STrack *)&contentLink->getSObject() );
                }
            }
        }
        childNode = childNode.nextSibling();
    }
                
    return new SLink( *mixer, parent );
}
