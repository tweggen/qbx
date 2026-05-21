
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
    return this->children().count();
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
#if 1
    const QObjectList& children = this->children();
    return (SLink*) (const_cast<QObjectList*>(&children)->at( idx ));
#else
    SSMPerTrack *spt = getPerTrackAt( idx );
    if( !spt ) return NULL;    
    SLink *lk = &(spt->getTrack());
    return lk;
#endif
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
    const QObjectList& children = this->children();
    for( auto it=children.cbegin(); it != children.cend(); ++it ) {
        SLink *lk = const_cast<SLink*>((const SLink*) *it);
        if( lk ) {
            lk->getSObject().seekTo( off );
        } else {
            qWarning( "SStdMixer::seekTo(): Link was NULL.\n" );
        }
    }
    return 0;
}

/**
 * Remove all inputs from each of the mixers.
 * Add all track outputs again to each of the bus mixers.
 */
void SStdMixer::reconnectTracksToMixer()
{
    const QObjectList& children = this->children();
    if( !cpMixers_ ) {
        qWarning( "Should reconnect tracks although no mixer was created yet.\n" );
        return;
    }
    qWarning( "Reconnecting tracks; %d busses.\n", nBusses_ );
    int nTracks = children.count();
    // For all busses.
    for( int bus=0; bus<nBusses_; bus++ ) {
        twMixer *mix = cpMixers_[bus];
        if( !mix ) continue;
        // Ensure the given number of inputs.
        mix->setNInputs( nTracks );
        // FIXME: Remove this ugly iterating.
        int channel;
        for( channel=0; channel<nTracks; channel++ ) {
            SLink *lk = NULL;
            lk = (SLink *)(const_cast<QObjectList&>(children)).at( channel );
            if( !lk ) {
                mix->setInput( channel, NULL );
                mix->setInputLevel( channel, 0 );
            } else {
                twComponent &root = lk->getRootComponent();
                qWarning( "Link Track $%x root component = $%x.\n", 
                          (unsigned)(ptrdiff_t)lk,
                          (unsigned)(ptrdiff_t) &root );                
                qWarning( "Calling $%08x->setInput( %d, "
                          "$%08x->getRootComponent().linkOutput( %d ) );\n",
                          (unsigned)(ptrdiff_t) mix,
                          channel, (unsigned)(ptrdiff_t) lk, bus );
                mix->setInput( channel, root.linkOutput( bus ) );
                mix->setInputLevel( channel, lk->getSObject().getVolume() );
            }
        }
        // FIXME: Clear the remaining channels.
    }
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
    const QObjectList& children = this->children();
    int nTracks = children.count();
    
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
        twMixer *mix;
        // FIXME: proper n channels.
        qWarning( "SStdMixer::setNBusses( %d ): Creating new mixer #%d.\n", n, i );
        int nc = nTracks;
        if( nc<1 ) nc = 1;
        mix = new twMixer( *(SApplication::app().get303aEnvironment()), nc );
        mix->init();
        cpRewire_->setInput( i, mix->linkOutput( 0 ) );

        SLink *st = (SLink *)(const_cast<QObjectList *>(&children)->at( i ));
        if( st ) {
            mix->setInputLevel( i, st->getSObject().getVolume() );
            QObject::connect( 
                &(st->getSObject()), SIGNAL( volumeChanged( double ) ), 
                this, SLOT( trackVolumeChanged( double ) ) );
        } else {
            mix->setInputLevel( i, 0 );
        }
        newMixers[i] = mix;
    }
    nBusses_ = n;
    cpMixers_ = newMixers;
    if( oldMixers ) ::free( oldMixers );
    return 0;
}

/**
 * This function is meant for internal use only.
 */
void SStdMixer::trackVolumeChanged( double  )
{
    qWarning( "SStdMixer::trackVolumeChanged(): called." );
    SObject *so = (SObject *) (const SObject *) sender();
    if( !so ) {
        qWarning( "No per track object found.\n" );
        return;
    }
    // FIXME: ALso consider pan.
    double cvol = so->getVolume();
    int i = getChildIndex( (SObject &)(*so) );
    if( i<0  /* || i>=(int)trackList_.count() */ ) {
        qWarning( "Invalid track index returned.\n" );
        return;
    }
    for( int bus=0; bus<nBusses_; bus++ ) {    
        twMixer *m = cpMixers_[bus];
        if( !m ) continue;
        m->setInputLevel( i, cvol );
    }
}

void SStdMixer::insertTrack( int newIndex, STrack &trk )
{
    const QObjectList& children = this->children();
    int nKids = children.count();
    if( newIndex<0 ) newIndex = nKids;
    qWarning( "Inserting new track @%d.\n", newIndex );
    QObject::connect( (QObject*)&trk, SIGNAL( durationChanged( length_t ) ), 
                      this, SLOT( mixerChildDurationChanged( length_t ) ) );    
    QObject::connect( 
        (QObject*)&trk, SIGNAL( volumeChanged( double ) ), 
        this, SLOT( trackVolumeChanged( double ) ) );
    SLink *lk = new SLink( (SObject&)trk, this );
    (void) lk;
    emit trackInserted( newIndex, trk );
}

int SStdMixer::removeTrack( SLink &track )
{
    const QObjectList& children = this->children();
    int nKids = children.count();
    int idx = getChildIndex( track.getSObject() );
    if( idx<0 || idx>=nKids ) return -1;
    SLink *sl = (SLink *) (const_cast<QObjectList*>(&children)->at( idx ));
    if( !sl ) return -1;
    QObject::disconnect( &(sl->getSObject()), SIGNAL( durationChanged( length_t ) ), 
                         this, SLOT( mixerChildDurationChanged( length_t ) ) );    
    QObject::disconnect( &(sl->getSObject()), SIGNAL( volumeChanged( double ) ), 
                         this, SLOT( trackVolumeChanged( double ) ) );
    return removeTrack( idx );
}

int SStdMixer::removeTrack( int trackIndex )
{    
    const QObjectList& children = this->children();
    int nKids = children.count();
    // FIXME: This is inefficient.
    SLink *stl;
    if( trackIndex<0 || trackIndex>=nKids ) return -1;
    // stl = (SLink *)((QObjectList *)children)->at( trackIndex );
    stl = (SLink *)(const_cast<QObjectList *>(&children)->at( trackIndex ));
    if( !stl ) {
        qWarning( "SStdMixer::removeTrack(): Child requested was not found.\n" );
        return -1;
    }
    SObject &so = stl->getSObject();
    emit trackRemoved( trackIndex, (STrack &)so );
    delete stl;
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
    setNBusses( 0 );
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
            qWarning() << "found SStdMixer child " << childNode.nodeName() << endl;
            if( childNode.nodeName() == "SLink" ) {
                QDomElement childElement = childNode.toElement();
                QString objectId = childElement.attribute( "objectId" );
                // Look up the object id.
                contentLink = projectLoader.getObjectDictionary().value( objectId );
                if( contentLink ) {
                    // FIXME: Check, wether this is a track, or create a generic insertion function.
                    mixer->insertTrack( -1, *(STrack *)&contentLink->getSObject() );
                }
            }
        }
        childNode = childNode.nextSibling();
    }
                
    return new SLink( *mixer, parent );
}
