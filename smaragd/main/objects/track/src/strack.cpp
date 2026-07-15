

#include <QDebug>

#include <iostream>

#include <stdlib.h>
#include <vector>

#include <qobject.h>

#include "tw/mix/twtrackmix.h"
#include "tw/mix/twrewire.h"
#include "tw/plugins/twpluginchain.h"
#include "app/model/sobject.h"
#include "app/model/sproject.h"
#include "app/model/slink.h"
#include "app/model/sappcontext.h"
#include "app/objects/track/strack.h"
#include "app/objects/track/strackrndrinline.h"
#include "app/objects/track/spluginchain.h"
#include "app/objects/track/spluginslot.h"
#include "app/persistence/sprojectloader.h"
#include "tw/schedule/capture_aspects.h"  // Preview/Playback/... bits

using namespace std;

int STrack::serializeSelfAttributes( QTextStream &o )
{
    o << " nBusses='" << getNBusses() << "'";
    SObject::serializeSelfAttributes( o );
    return 0;
}

QWidget *STrack::getDetailEditWidget( QWidget * )
{
    return NULL;
}

QWidget *STrack::getInlineEditWidget( QWidget * )
{
    return NULL;
}

SObjectRenderer *STrack::getInlineRenderer()
{
    if( !inlineRenderer_ ) {
        inlineRenderer_ = new STrackRendererInline( *this );
    }
    return inlineRenderer_;
}

/**
 * We define a track's duration as the ending point of the last terminated event,
 * otherwise 1.
 */
bool STrack::hasDuration() const
{
    return true;
}

/**
 * We define a track's duration as the ending point of the last terminated event,
 * otherwise 1.
 */
length_t STrack::getDuration() const
{
    if( !lastDurationValid_ ) {
        offset_t first, last = 0;
        int nUndefStart, nUndefDuration;
        getChildrenExtent( first, last, nUndefStart, nUndefDuration );
	//	qWarning( "STrack::getDuration(): last = %d.\n", (int) last );
        if( last>0 ) {
            lastDuration_ = last;
        } else {
            lastDuration_ = 1;
        }
        lastDurationValid_ = true;
    }
    return lastDuration_;
}

std::shared_ptr<twComponent> STrack::getRootComponent()
{
    return std::static_pointer_cast<twComponent>(cpRewire_);
}

std::shared_ptr<twComponent> STrack::getCaptureComponent() const
{
    if( nBusses_ > 0 && cpTrackMixers_[0] )
        return std::static_pointer_cast<twComponent>(cpTrackMixers_[0]);
    return std::shared_ptr<twComponent>();
}

int STrack::seekTo( offset_t ofs )
{
    for( int i=0; i<nBusses_; i++ ) {
        std::shared_ptr<twTrackMix> mix = cpTrackMixers_[i];
        if( mix ) mix->seekTo( ofs );
    }
    return 0;
}

void STrack::bumpRenderChainEpoch()
{
    for( int i=0; i<nBusses_; ++i ) {
        if( cpTrackMixers_[i] )
            cpTrackMixers_[i]->bumpContentEpoch();
        if( cpPluginChains_[i] )
            cpPluginChains_[i]->bumpContentEpoch();   // forwards to inserts
    }
    if( cpRewire_ )
        cpRewire_->bumpContentEpoch();
}

SLink *STrack::getTopMostSLinkAt( offset_t queryTime ) const
{
    for( SLink *lk : childLinks() ) {
        // Skip child tracks — they have their own lanes; this query is for the
        // track's own clips.
        if( dynamic_cast<STrack*>( &lk->getSObject() ) ) continue;
        if( !lk->hasStartTime() ) continue;
        offset_t startTime = lk->getStartTime();
        if( queryTime<startTime ) continue;
        if( lk->getSObject().hasDuration() ) {
            offset_t endTime = startTime+lk->getSObject().getDuration();
            if( queryTime<endTime ) return lk;
        } else {
            return lk;
        }
    }
    return NULL;
}

void STrack::checkDurationChanged()
{
    length_t oldDuration = lastDuration_;
    length_t newDuration;
    newDuration = getDuration();
    qWarning( "STrack::checkDurationChanged() called. oldDuration = %d, newDuration = %d.\n",
              (int)oldDuration, (int)newDuration );
    qWarning( "STrack::checkDurationChanged(): newDuration = %d:%d.",
	      (int)(newDuration>>32), (int)newDuration );
    if( newDuration!=oldDuration ) {
        emit durationChanged( newDuration );
    }
}

void STrack::trackChildDurationChanged( length_t newLength )
{
    // durationChanged is connected on the child's OBJECT (see
    // trackChildWasAdded), so sender() is the SObject — an SCut, say — not the
    // SLink. The old dynamic_cast<SLink*>(sender()) was therefore always null,
    // and the engine's clip window silently kept its stale duration (a split
    // clip's head kept sounding over its full pre-split span). Resolve every
    // link of ours that references the sender object and update each placement.
    SObject *obj = dynamic_cast<SObject *>( sender() );
    if( obj ) {
        for( SLink *lk : childLinks() ) {
            if( !lk || &lk->getSObject() != obj ) continue;
            for( int i=0; i<nBusses_; ++i ) {
                if( cpTrackMixers_[i] ) {
                    cpTrackMixers_[i]->updateClip(lk, lk->getStartTime(), newLength);
                }
            }
        }
        // AFTER the engine mutation: stale this chain and every container up
        // to the root (scoped — siblings keep their caches).
        invalidateRenderPath();
    }
    lastDurationValid_ = false;
    checkDurationChanged();
}

void STrack::trackChildWasMoved( offset_t newTime )
{
    SLink *slink = (SLink *) (const SLink *) sender();
    if( slink && slink->getSObject().hasDuration() ) {
        length_t duration = slink->getSObject().getDuration();
        for( int i=0; i<nBusses_; ++i ) {
            if( cpTrackMixers_[i] ) {
                cpTrackMixers_[i]->updateClip(slink, newTime, duration);
            }
        }
        invalidateRenderPath();
        lastDurationValid_ = false;
        checkDurationChanged();
    }
}

/**
 * We have a new child. Insert it into the clip list on all track mixers.
 */
void STrack::trackChildWasAdded( SLink &child )
{
    if( child.hasStartTime() ) {
        if( child.getSObject().hasDuration() ) {
            // If we have a new child, with a duration, attach a callback
            // to it, which informs us, if its starttime changes.
            QObject::connect( &child, SIGNAL( startTimeChanged( offset_t ) ),
                              this, SLOT( trackChildWasMoved( offset_t ) ) );
            QObject::connect( &(child.getSObject()), SIGNAL( durationChanged( length_t ) ),
                              this, SLOT( trackChildDurationChanged( length_t ) ) );

            // Insert the clip into all track mixers with a callback that gets the component
            offset_t startTime = child.getStartTime();
            length_t duration = child.getSObject().getDuration();
            // Capture SLink by reference; callback will call getRootComponent() dynamically
            auto getComponentFn = [&child]() { return child.getRootComponent(); };
            // Position mapper: folds a windowed clip's slip offset into
            // clip-relative positions before the component is seeked/frozen.
            auto mapPosFn = [&child]( offset_t off ) {
                return child.getSObject().mapTimelineToComponentPos( off );
            };
            for( int i=0; i<nBusses_; ++i ) {
                if( cpTrackMixers_[i] ) {
                    cpTrackMixers_[i]->insertClip(&child, startTime, duration, getComponentFn, mapPosFn);
                }
            }
            invalidateRenderPath();

            lastDurationValid_ = false;
            checkDurationChanged();
        }
    }
}

void STrack::trackChildWasRemoved( SLink &child )
{
    if( child.hasStartTime() ) {
        if( child.getSObject().hasDuration() ) {
            // Remove the clip from all track mixers, keyed by the link itself.
            for( int i=0; i<nBusses_; ++i ) {
                if( cpTrackMixers_[i] ) {
                    cpTrackMixers_[i]->removeClip(&child);
                }
            }
            invalidateRenderPath();

            lastDurationValid_ = false;
            checkDurationChanged();
        }
    }
}

void STrack::setNBusses( int nBusses )
{
    if( nBusses==nBusses_ ) return;
    int oldNBusses = nBusses_;
    if( nBusses<oldNBusses ) {
        // Shrink not yet implemented; refuse rather than leaving stale wiring
        // that would cause a use-after-free on the next render.
        Q_ASSERT_X( false, "STrack::setNBusses", "bus count shrink not supported" );
        return;
    } else {
        // The number of busses is about to grow. Base the growth start index on
        // the actual container size, not nBusses_: the constructor sets
        // nBusses_==1 while leaving this vector empty, so trusting nBusses_ here
        // would skip creating bus 0 and leave a null shared_ptr behind.
        int oldMixerCount = (int)cpTrackMixers_.size();
        cpTrackMixers_.resize(nBusses);
        // Create the new ones.
        for( int i=oldMixerCount; i<nBusses; ++i ) {
            cpTrackMixers_[i] = std::make_shared<twTrackMix>(
                *(SAppContext::get().get303aEnvironment()) );
            cpTrackMixers_[i]->init();
        }
    }
    // Grow plugin chain array: keep existing chains, create new ones for added buses.
    {
        int oldChainCount = (int)cpPluginChains_.size();
        cpPluginChains_.resize(nBusses);

        // Create new plugin chain components for added buses only
        for( int i=oldChainCount; i<nBusses; ++i ) {
            cpPluginChains_[i] = std::make_shared<twPluginChain>(
                *(SAppContext::get().get303aEnvironment()), 1 );
            cpPluginChains_[i]->init();
        }
    }

    // Reset rewirer.
    if( cpRewire_ ) {
        for( int i=0;i<oldNBusses;++i ) {
            cpRewire_->setInput( i, NULL );
        }
    } else {
        cpRewire_ = std::make_shared<twRewire>( *(SAppContext::get().get303aEnvironment()) );
        cpRewire_->init();
    }
    cpRewire_->setNPlugs( nBusses );

    // Wire: track mixer → plugin chain → rewire
    for( int i=0; i<nBusses; ++i ) {
        cpPluginChains_[i]->setInput( 0, cpTrackMixers_[i]->linkOutput( 0 ) );
        // If this chain already has plugins, rebuildWiring() so it picks up the
        // (possibly new) input latch after setInput changed pInputPlugs[0].
        cpPluginChains_[i]->rebuildWiring();
        cpRewire_->setInput( i, cpPluginChains_[i]->linkOutput( 0 ) );
    }

    // Populate clip list in all track mixers with existing children (initial sync).
    // This runs on the UI thread, so it's safe to populate before audio starts.
    for( SLink *lk : childLinks() ) {
        if( !lk || !lk->hasStartTime() ) continue;
        offset_t startTime = lk->getStartTime();
        length_t duration = lk->getSObject().hasDuration() ? lk->getSObject().getDuration() : 0;
        // Create a callback that returns the component dynamically
        auto getComponentFn = [lk]() { return lk->getRootComponent(); };
        // Position mapper: folds a windowed clip's slip offset into
        // clip-relative positions before the component is seeked/frozen.
        auto mapPosFn = [lk]( offset_t off ) {
            return lk->getSObject().mapTimelineToComponentPos( off );
        };
        for( int i=0; i<nBusses; ++i ) {
            cpTrackMixers_[i]->insertClip(lk, startTime, duration, getComponentFn, mapPosFn);
        }
    }

    nBusses_ = nBusses;
    emit nChannelsChanged( nBusses );
}

STrack::STrack( SProject *project )
    : SObject( project ),
      inlineRenderer_( 0 ),
      nBusses_( 1 ),
      cpTrackMixers_( 0 ),
      cpPluginChain_( 0 ),
      cpPluginChains_( 0 ),
      lastDuration_( 1 ),
      lastDurationValid_( true )
{
    // Create the plugin chain model object (container for effect inserts)
    // NOTE: We do NOT call setParent(this) because the plugin chain is NOT an SLink.
    // SObject::childEvent() expects all children to be SLink instances; setting the
    // plugin chain as a Qt child would cause an invalid cast in childEvent().
    // Instead, we manage the chain's lifetime manually via the destructor.
    cpPluginChain_ = new SPluginChain( project );

    // Connect plugin chain model changes to DSP layer synchronization.
    QObject::connect( cpPluginChain_, SIGNAL( slotInserted( int, SPluginSlot & ) ),
                      this, SLOT( onPluginSlotInserted( int, SPluginSlot & ) ) );
    QObject::connect( cpPluginChain_, SIGNAL( slotRemoved( int, SPluginSlot & ) ),
                      this, SLOT( onPluginSlotRemoved( int, SPluginSlot & ) ) );
    QObject::connect( cpPluginChain_, SIGNAL( slotsReordered() ),
                      this, SLOT( onPluginSlotsReordered() ) );

    // Add a listener for added child objects.
    // We want to become noticed, if it is new.
    QObject::connect( this, SIGNAL( childObjectAdded( SLink & ) ),
                      this, SLOT( trackChildWasAdded( SLink & ) ) );
    QObject::connect( this, SIGNAL( childObjectRemoved( SLink & ) ),
                      this, SLOT( trackChildWasRemoved( SLink & ) ) );

    // Forward track mute and volume changes to the track mixers
    QObject::connect( this, SIGNAL( mutedChanged( bool ) ),
                      this, SLOT( onTrackMuteChanged( bool ) ) );
    QObject::connect( this, SIGNAL( volumeChanged( double ) ),
                      this, SLOT( onTrackVolumeChanged( double ) ) );

    // Set the number of busses. This initial request will allocate
    // the track mixer objects and DSP plugin chains.
    setNBusses( 2 );
}

STrack::~STrack()
{
    DTOR_DEL( inlineRenderer_ );
    // NOTE: cpPluginChain_ is a Qt child of the project, so it will be deleted
    // automatically by Qt's parent-child cleanup. Do NOT manually delete it here
    // to avoid double-delete crashes during project destruction.
    // (Historically it was manually managed, but current design makes it a project child.)

    cpPluginChains_.resize(0);
    cpTrackMixers_.resize(0);
    cpRewire_.reset();
}

#if 0
int SEndTimeList::compareItems( QCollection::Item item1, QCollection::Item item2 )
{    
    SLink *so1 = (SLink *)item1;
    SLink *so2 = (SLink *)item2;    
    offset_t endTime1 = so1->getStartTime() + so1->getSObject().getDuration();
    offset_t endTime2 = so2->getStartTime() + so2->getSObject().getDuration();
    if( endTime1==endTime2 ) return 0;
    if( endTime1<endTime2 ) return -1;
    return 1;
}

int SStartTimeList::compareItems( QCollection::Item item1, QCollection::Item item2 )
{    
    SLink *so1 = (SLink *)item1;
    SLink *so2 = (SLink *)item2;    
    offset_t startTime1 = so1->getStartTime();
    offset_t startTime2 = so2->getStartTime();
    if( startTime1==startTime2 ) return 0;
    if( startTime1<startTime2 ) return -1;
    return 1;
}
#endif

SEndTimeList::SEndTimeList()
{
}

SEndTimeList::~SEndTimeList()
{
}

SStartTimeList::SStartTimeList()
{
}

SStartTimeList::~SStartTimeList()
{
}

int STrack::readPreChildrenAttributes( QDomElement &element )
{
    SObject::readPreChildrenAttributes( element );
    
    QString data;
    data = element.attribute( "nBusses", "1" );
    setNBusses( data.toInt() );
    
    return 0;
}

SLink *STrack::instantiateFromDomElement(
    SProjectLoader &projectLoader, QDomElement &element, SObject *parent )
{
    (void) parent;
    SLink *contentLink = NULL;
    // Find the first link child
    QDomNode childNode = element.firstChild();
    STrack *track = new STrack( &projectLoader.getProject() );
    track->readPreChildrenAttributes( element );
    while( !childNode.isNull() ) {
        if( childNode.isElement() ) {
            qWarning() << "found STrack child " << childNode.nodeName() << Qt::endl;
            if( childNode.nodeName() == "SLink" ) {
                QDomElement childElement = childNode.toElement();
                QString objectId = childElement.attribute( "objectId" );
                // Look up the object id.
                contentLink = projectLoader.getObjectDictionary().value( objectId );
                if( contentLink ) {
                    // FIXME: Check, wether this is a track, or create a generic insertion function.
                    SLink *sl = new SLink( contentLink->getSObject(), NULL );
                    if( sl ) {
                        sl->readAttributes( childElement );
                        sl->setParent(track); // was: track->insertChild( sl );
                    } else {
                        qWarning() << "Failed to create SLink for object" << objectId;
                    }
                } else {
                    qWarning() << "Object not found in dictionary:" << objectId;
                }
            }
        }
        childNode = childNode.nextSibling();
    }
    track->readPostChildrenAttributes( element );
    return new SLink( *track );
}

void STrack::onPluginSlotInserted( int index, SPluginSlot &slot )
{
    // Sync the model change to all DSP plugin chains
    // Pre-allocate inserts for all buses to ensure they're fully initialized
    // before the audio thread accesses them
    if( nBusses_ > 0 ) {
        // First, ensure all inserts exist and are fully initialized
        for( int i = 0; i < nBusses_; ++i ) {
            std::shared_ptr<audio::twPluginInsert> insert = slot.getInsertForBus(i);
            if( !insert ) {
                // Insert creation failed - the slot will handle the error
                return;
            }
        }

        // Now that all inserts are safely created, add them to the chains
        for( int i = 0; i < nBusses_; ++i ) {
            if( cpPluginChains_[i] ) {
                std::shared_ptr<audio::twPluginInsert> insert = slot.getInsertForBus(i);
                if( insert ) {
                    cpPluginChains_[i]->addPlugin( insert );
                }
            }
        }
        invalidateRenderPath();
    }
}

void STrack::onPluginSlotRemoved( int index, SPluginSlot &slot )
{
    // Sync the model change to all DSP plugin chains
    // Note: we remove by index since the slot is being deleted
    {
        for( int i = 0; i < nBusses_; ++i ) {
            if( cpPluginChains_[i] ) {
                cpPluginChains_[i]->removePlugin( index );
            }
        }
        invalidateRenderPath();
    }
}

void STrack::onPluginSlotsReordered()
{
    // Sync plugin reordering to all DSP plugin chains
    {
        for( int i = 0; i < nBusses_; ++i ) {
            if( cpPluginChains_[i] ) {
                cpPluginChains_[i]->rebuildWiring();
            }
        }
        invalidateRenderPath();
    }
}

// Phase 5e: Page cache implementation
void STrack::onTrackMuteChanged( bool muted )
{
    // Forward mute change to all track mixers
    for( int i=0; i<nBusses_; ++i ) {
        if( cpTrackMixers_[i] ) {
            cpTrackMixers_[i]->setTrackMute(muted);
        }
    }
    // Mute is baked into frozen pages downstream of the track mixer
    invalidateRenderPath();
}

void STrack::onTrackVolumeChanged( double gainDb )
{
    // Forward volume change to all track mixers
    for( int i=0; i<nBusses_; ++i ) {
        if( cpTrackMixers_[i] ) {
            cpTrackMixers_[i]->setTrackGain(gainDb);
        }
    }
    // Gain is baked into frozen pages downstream of the track mixer
    invalidateRenderPath();
}

// Self-registration with the project loader (proposal 14, Phase 5): the
// persistence module names no concrete types; each slice registers its own
// element name. Relies on the app being an OBJECT library (no TU elision).
static const bool s_registered_strack =
    ( SProjectLoader::registerSObjectClass( "STrack",
          STrack::instantiateFromDomElement ), true );
