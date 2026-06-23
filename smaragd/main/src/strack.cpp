

#include <QDebug>

#include <iostream>

#include <stdlib.h>
#include <vector>

#include <qobject.h>

#include "twtrackmix.h"
#include "twrewire.h"
#include "twpluginchain.h"
#include "sobject.h"
#include "sproject.h"
#include "slink.h"
#include "sapplication.h"
#include "strack.h"
#include "strackrndrinline.h"
#include "spluginchain.h"
#include "spluginslot.h"
#include "sprojectloader.h"
#include "scut.h"  // For SCutCaptureAspect enum (Preview)

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

twComponent &STrack::getRootComponent()
{
    return *(twComponent *)cpRewire_;
}

twComponent *STrack::getCaptureComponent() const
{
    if( cpTrackMixers_ && nBusses_ > 0 && cpTrackMixers_[0] )
        return (twComponent *) cpTrackMixers_[0];
    return NULL;
}

int STrack::seekTo( offset_t ofs )
{
    if( !cpTrackMixers_ ) return 0;
    for( int i=0; i<nBusses_; i++ ) {
        twTrackMix *mix = cpTrackMixers_[i];
        if( mix ) mix->seekTo( ofs );
    }
    return 0;
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

void STrack::trackChildDurationChanged( length_t /* newLength*/ )
{
    // SObject *sendingObject = (SObject *) (const SObject *) sender();
    lastDurationValid_ = false;
    checkDurationChanged();
}

void STrack::trackChildWasMoved( offset_t /*newTime*/ )
{
    SLink *sendingObject = (SLink *) (const SLink *) sender();
    // For now, do nothing, as we don't need anything to be done here.
    // Look, if the moved object leeds to a larger end time.
    if( sendingObject->getSObject().hasDuration() ) {
        lastDurationValid_ = false;
        checkDurationChanged();
    }
}

/**
 * We have a new child. Check, if we need to insert it into the starttime list.
 */
void STrack::trackChildWasAdded( SLink &child )
{
    // SObject *sendingObject = (SObject *) (const SObject *) sender();
    
    if( child.hasStartTime() ) {
//      startTimeList_.inSort( &child );
        if( child.getSObject().hasDuration() ) {
//          endTimeList_.inSort( &child );
            // If we have a new child, with a duration, attach a callback
            // to it, which informs us, if its
            // starttime changes.
            QObject::connect( &child, SIGNAL( startTimeChanged( offset_t ) ), 
                              this, SLOT( trackChildWasMoved( offset_t ) ) );    
            QObject::connect( &(child.getSObject()), SIGNAL( durationChanged( length_t ) ), 
                              this, SLOT( trackChildDurationChanged( length_t ) ) );    
            lastDurationValid_ = false;
            checkDurationChanged();
        }
    }
}

void STrack::trackChildWasRemoved( SLink &/*child*/ )
{
    // FIXME: Unfortunately, we cannot access the child here.
#if 0
    SObject *sendingObject = (SObject *) (const SObject *) sender();
    // We can try to remove it, regardless, wether we had it or not.
//    startTimeList_.remove( child );
//    endTimeList_.remove( child );
    if( child.hasStartTime() ) {
//    startTimeList_.inSort( &child );
        if( child.getSObject().hasDuration() ) {
            QObject::disconnect( &child, SIGNAL( startTimeChanged( offset_t ) ), 
                              this, SLOT( trackChildWasMoved( offset_t ) ) );    
            QObject::disconnect( &(child.getSObject()), SIGNAL( durationChanged( length_t ) ), 
                              this, SLOT( trackChildDurationChanged( length_t ) ) );    
            lastDurationValid_ = false;
            checkDurationChanged();
        }
    }
#else
    lastDurationValid_ = false;
    checkDurationChanged();
#endif
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
        // The number of busses is about to grow.
        twTrackMix **newMixers = (twTrackMix **) ::calloc( 
            sizeof( twTrackMix * ), nBusses );
        if( cpTrackMixers_ ) {
            // We formerly had some allocated. Copy them.
            for( int i=0; i<oldNBusses; ++i ) {
                newMixers[i] = cpTrackMixers_[i];
            }
        } else {
            oldNBusses = 0;
        }
        // Create the new ones.
        for( int i=oldNBusses; i<nBusses; ++i ) {
            newMixers[i] = new twTrackMix(
                *(SApplication::app().get303aEnvironment()), 
                *this );
            newMixers[i]->init();
        }
        ::free( cpTrackMixers_ );
        cpTrackMixers_ = newMixers;
    }
    // Grow plugin chain array: keep existing chains, create new ones for added buses.
    {
        int chainOldN = cpPluginChains_ ? oldNBusses : 0;
        twPluginChain **newChains = (twPluginChain **) ::calloc(
            sizeof( twPluginChain * ), nBusses );
        // Preserve existing chains — they are already wired and may hold audio state
        for( int i=0; i<chainOldN; ++i ) {
            newChains[i] = cpPluginChains_[i];
        }
        // Create new plugin chain components for added buses only
        for( int i=chainOldN; i<nBusses; ++i ) {
            newChains[i] = new twPluginChain(
                *(SApplication::app().get303aEnvironment()), 1 );
            newChains[i]->init();
        }
        ::free( cpPluginChains_ );
        cpPluginChains_ = newChains;
    }

    // Reset rewirer.
    if( cpRewire_ ) {
        for( int i=0;i<oldNBusses;++i ) {
            cpRewire_->setInput( i, NULL );
        }
    } else {
        cpRewire_ = new twRewire( *(SApplication::app().get303aEnvironment()) );
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
    nBusses_ = nBusses;
    emit nChannelsChanged( nBusses );
}

STrack::STrack( SProject *project )
    : SObject( project ),
      inlineRenderer_( 0 ),
      nBusses_( 1 ),
      cpTrackMixers_( 0 ),
      cpRewire_( 0 ),
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

    if( cpPluginChains_ ) {
        for( int i = 0; i < nBusses_; ++i ) {
            delete cpPluginChains_[i];
        }
        ::free( cpPluginChains_ );
    }
    if( cpTrackMixers_ ) {
        for( int i = 0; i < nBusses_; ++i ) {
            delete cpTrackMixers_[i];
        }
        ::free( cpTrackMixers_ );
    }
    delete cpRewire_;
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
    if( cpPluginChains_ && nBusses_ > 0 ) {
        // First, ensure all inserts exist and are fully initialized
        for( int i = 0; i < nBusses_; ++i ) {
            audio::twPluginInsert *insert = slot.getInsertForBus(i);
            if( !insert ) {
                // Insert creation failed - the slot will handle the error
                return;
            }
        }

        // Now that all inserts are safely created, add them to the chains
        for( int i = 0; i < nBusses_; ++i ) {
            if( cpPluginChains_[i] ) {
                audio::twPluginInsert *insert = slot.getInsertForBus(i);
                if( insert ) {
                    cpPluginChains_[i]->addPlugin( insert );
                }
            }
        }
    }
}

void STrack::onPluginSlotRemoved( int index, SPluginSlot &slot )
{
    // Sync the model change to all DSP plugin chains
    // Note: we remove by index since the slot is being deleted
    if( cpPluginChains_ ) {
        for( int i = 0; i < nBusses_; ++i ) {
            if( cpPluginChains_[i] ) {
                cpPluginChains_[i]->removePlugin( index );
            }
        }
    }
}

void STrack::onPluginSlotsReordered()
{
    // Sync plugin reordering to all DSP plugin chains
    if( cpPluginChains_ ) {
        for( int i = 0; i < nBusses_; ++i ) {
            if( cpPluginChains_[i] ) {
                cpPluginChains_[i]->rebuildWiring();
            }
        }
    }
}

// Phase 5e: Page cache implementation
void STrack::recomputePreview(CapturePageData& page)
{
    // Composite preview: gather previews from all visible children and mix them

    length_t totalDuration = getDuration();
    if (totalDuration == 0) {
        // Empty track; fill with silence
        preview_t* previewData = reinterpret_cast<preview_t*>(page.data);
        for (size_t i = 0; i < CapturePageData::PAGE_SIZE / sizeof(preview_t); ++i) {
            previewData[i].min = 0;
            previewData[i].max = 0;
        }
        page.validAspects |= Preview;
        return;
    }

    const offset_t MAX_PREVIEW_SAMPLES = CapturePageData::PAGE_SIZE / sizeof(preview_t);
    preview_t* pagePreview = reinterpret_cast<preview_t*>(page.data);

    // Initialize composite buffer to silence
    for (size_t i = 0; i < MAX_PREVIEW_SAMPLES; ++i) {
        pagePreview[i].min = 0;
        pagePreview[i].max = 0;
    }

    // Iterate through all child clips
    for (SLink *childLink : childLinks()) {
        if (!childLink) continue;

        SObject *child = &childLink->getSObject();
        if (!child || child->isMuted()) {
            // Skip muted or missing children
            continue;
        }

        // Try to get preview from this child
        if (child->hasPreview()) {
            std::vector<preview_t> childPreview(MAX_PREVIEW_SAMPLES);
            int result = child->getPreview(childPreview.data(), 0, totalDuration, MAX_PREVIEW_SAMPLES);

            if (result >= 0) {
                // Mix this child's preview into the composite
                // For each sample slot: expand the min/max range to include child's range
                for (offset_t i = 0; i < MAX_PREVIEW_SAMPLES; ++i) {
                    if (childPreview[i].min < pagePreview[i].min) {
                        pagePreview[i].min = childPreview[i].min;
                    }
                    if (childPreview[i].max > pagePreview[i].max) {
                        pagePreview[i].max = childPreview[i].max;
                    }
                }
            }
        }
    }

    // Mark preview as valid in the page
    page.validAspects |= Preview;
}
