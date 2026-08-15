

#include <QDebug>

#include <iostream>

#include <stdlib.h>
#include <cstdint>
#include <vector>

#include <qobject.h>

#include "tw/mix/twtrackmix.h"
#include "tw/mix/twrewire.h"
#include "tw/plugins/twpluginchain.h"
#include "app/model/sobject.h"
#include "app/model/sproject.h"
#include "app/model/slink.h"
#include "app/model/sappcontext.h"
#include "app/model/splacements.h"
#include "app/model/ssolorules.h"
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
    // Which <SPluginChain> is OURS (proposal 08 M4). Every SObject is a child of
    // the project and is serialized from there, so a chain lands in the file
    // whether or not anyone claims it; before M4 nothing did, and a loaded chain
    // (with all its slots) was orphaned — the constructor's fresh empty chain won
    // and ~SProjectLoader dropped the loaded one to refcount 0.
    //
    // A plain attribute, not an <SLink> child: SObject::childEvent treats every
    // child link as a clip placement, and the loader's dependency ordering is
    // built on child links — which is exactly why the load side has to defer
    // (see instantiateFromDomElement).
    if( cpPluginChain_ )
        o << " pluginChainId='"
          << reinterpret_cast<std::uintptr_t>( (SObject *) cpPluginChain_ ) << "'";
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

int STrack::seekTo( offset_t ofs )
{
    for( int i=0; i<nBusses_; i++ ) {
        std::shared_ptr<twTrackMix> mix = cpTrackMixers_[i];
        // seek(): app model → engine component, an EXTERNAL seek (see
        // twComponent::seek). The mix's own clip cascade below it stays on
        // seekTo(), being internal to the mix.
        if( mix ) mix->seek( ofs );
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

// Range-scoped variant (proposal 18 Phase 5): every component in the chain
// speaks the same timeline positions, so one range fits all of them.
void STrack::bumpRenderChainEpochRange( offset_t start, offset_t end )
{
    for( int i=0; i<nBusses_; ++i ) {
        if( cpTrackMixers_[i] )
            cpTrackMixers_[i]->invalidatePagesInRange(start, end);
        if( cpPluginChains_[i] )
            cpPluginChains_[i]->invalidatePagesInRange(start, end);   // forwards to inserts
    }
    if( cpRewire_ )
        cpRewire_->invalidatePagesInRange(start, end);
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
        twEditRange affected;
        for( SLink *lk : childLinks() ) {
            if( !lk || &lk->getSObject() != obj ) continue;
            for( int i=0; i<nBusses_; ++i ) {
                if( cpTrackMixers_[i] ) {
                    twEditRange r = cpTrackMixers_[i]->updateClip(
                        lk, lk->getStartTime(), newLength);
                    affected.unite(r.start, r.end);
                }
            }
        }
        // AFTER the engine mutation: stale this chain and every container up
        // to the root over EXACTLY the affected extent (union of the pre-
        // and post-edit clip windows, reported by the mix) — pages elsewhere
        // in the song survive (proposal 18 Phase 5).
        invalidateRenderPathRange( (offset_t) affected.start,
                                   (offset_t) affected.end );
    }
    lastDurationValid_ = false;
    checkDurationChanged();
}

void STrack::trackChildWasMoved( offset_t newTime )
{
    SLink *slink = (SLink *) (const SLink *) sender();
    if( slink && slink->getSObject().hasDuration() ) {
        // Blocking read — a stale try-lock duration here would resize the clip
        // window wrongly via updateClip (same class as the insert-path fix).
        length_t duration = slink->getSObject().getDurationBlocking();
        twEditRange affected;
        for( int i=0; i<nBusses_; ++i ) {
            if( cpTrackMixers_[i] ) {
                twEditRange r = cpTrackMixers_[i]->updateClip(slink, newTime, duration);
                affected.unite(r.start, r.end);
            }
        }
        // Union of the old and new placements (the mix knew the old one).
        invalidateRenderPathRange( (offset_t) affected.start,
                                   (offset_t) affected.end );
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
            // BLOCKING read (proposal 19): getDuration()'s try-lock snapshot can
            // return a fresh cut's DEFAULT (0) when a revalidation worker holds
            // the cut mutex (the just-set duration scheduled a Preview job).
            // duration=0 inserts an UNBOUNDED clip that bleeds source past the
            // clip end (takes_recording_placement doubling). Edit path — may block.
            length_t duration = child.getSObject().getDurationBlocking();
            // Capture SLink by reference; callback will call getRootComponent() dynamically
            auto getComponentFn = [&child]() { return child.getRootComponent(); };
            // Inv-1: single resolver — component + slip-folded position from ONE
            // clip snapshot, so the freeze can't straddle a lazy reader build.
            auto resolveFn = [&child]( offset_t off ) {
                return child.getSObject().resolveClip( off );
            };
            twEditRange affected;
            for( int i=0; i<nBusses_; ++i ) {
                if( cpTrackMixers_[i] ) {
                    twEditRange r = cpTrackMixers_[i]->insertClip(
                        &child, startTime, duration, getComponentFn, resolveFn);
                    affected.unite(r.start, r.end);
                }
            }

            // We are the summing parent for a child TRACK (a folder lane), so
            // its audibility is ours to enforce — a nested track is not a mixer
            // child, so SStdMixer's null-the-input-plug path never sees it. Seed
            // the entry from the child's CURRENT state under the shared rule: a
            // track that is already muted (or soloed out) when it is reparented
            // in must land silent.
            if( STrack *childTrack = dynamic_cast<STrack*>( &child.getSObject() ) ) {
                QObject::connect( childTrack, SIGNAL( mutedChanged( bool ) ),
                                  this, SLOT( childTrackMuteChanged( bool ) ),
                                  Qt::UniqueConnection );
                // Solo is global: relay it up to the root mixer rather than
                // resolving it here (see childTrackSoloChanged). Both hops are
                // needed — the direct flag of this child, and anything a nested
                // folder below it forwards.
                QObject::connect( childTrack, SIGNAL( soloChanged( bool ) ),
                                  this, SLOT( childTrackSoloChanged() ),
                                  Qt::UniqueConnection );
                QObject::connect( childTrack, SIGNAL( subtreeSoloChanged() ),
                                  this, SLOT( childTrackSoloChanged() ),
                                  Qt::UniqueConnection );
                SObject *root = splacements::rootContainer( getProjectSafe() );
                if( !ssolo::isLaneAudible( root, childTrack ) ) {
                    for( int i=0; i<nBusses_; ++i ) {
                        if( cpTrackMixers_[i] ) {
                            twEditRange r =
                                cpTrackMixers_[i]->setClipMuted( &child, true );
                            affected.unite(r.start, r.end);
                        }
                    }
                }
            }
            // Only the new clip's extent changed (proposal 18 Phase 5).
            invalidateRenderPathRange( (offset_t) affected.start,
                                       (offset_t) affected.end );

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
            twEditRange affected;
            for( int i=0; i<nBusses_; ++i ) {
                if( cpTrackMixers_[i] ) {
                    twEditRange r = cpTrackMixers_[i]->removeClip(&child);
                    affected.unite(r.start, r.end);
                }
            }
            // Only the removed clip's extent went silent (proposal 18 Phase 5).
            invalidateRenderPathRange( (offset_t) affected.start,
                                       (offset_t) affected.end );

            lastDurationValid_ = false;
            checkDurationChanged();
        }
    }
}

void STrack::setNBusses( int nBusses )
{
    // Hoisted: the initial-sync loop at the end needs to know which mixers
    // are NEW, so it does not re-insert clips into ones that already hold them.
    int oldMixerCount = 0;
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
        oldMixerCount = (int)cpTrackMixers_.size();
        cpTrackMixers_.resize(nBusses);
        // Create the new ones.
        for( int i=oldMixerCount; i<nBusses; ++i ) {
            cpTrackMixers_[i] = std::make_shared<twTrackMix>(
                *(SAppContext::get().get303aEnvironment()) );
            cpTrackMixers_[i]->init();
        }
    }
    // Grow plugin chain array: keep existing chains, create new ones for added buses.
    int oldChainCount = (int)cpPluginChains_.size();
    {
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

    // Populate the NEWLY created chains with the existing slots' taps
    // (proposal 08 M3).
    //
    // This is the other half of the "a 2-in/2-out plugin gets silence on input
    // 1" fix. Before M3 each bus got its OWN plugin instance and a chain built
    // with nBusses_ == 1, whose rebuildWiring() could only ever wire port 0 —
    // so a stereo plugin's second input was never connected to anything. Now a
    // slot owns ONE processor and one 1-in/1-out TAP per bus, so a new bus needs
    // its own tap in its own chain, in slot order, and the processor gathers all
    // of them coherently. Slots also have to learn the final bus count BEFORE
    // any tap is built, because the count is what selects the channel-mismatch
    // mapping (direct / dual-mono / mono-fold / unsupported).
    if( cpPluginChain_ ) {
        const int nSlots = cpPluginChain_->getSlotCount();
        for( int s = 0; s < nSlots; ++s ) {
            if( SPluginSlot *slot = cpPluginChain_->getSlotAt( s ) )
                slot->setBusCount( nBusses );
        }
        for( int i = oldChainCount; i < nBusses; ++i ) {
            if( !cpPluginChains_[i] ) continue;
            for( int s = 0; s < nSlots; ++s ) {
                SPluginSlot *slot = cpPluginChain_->getSlotAt( s );
                if( !slot ) continue;
                if( auto tap = slot->getInsertForBus( i ) )
                    cpPluginChains_[i]->addPlugin( tap );
            }
        }
    }

    // Populate clip list in the NEW track mixers with existing children (initial
    // sync). This runs on the UI thread, so it's safe to populate before audio
    // starts.
    //
    // Only the mixers created by this call (index >= oldMixerCount): the ones
    // that already existed already hold every entry, and re-inserting would give
    // one SLink* two entries in the same mixer — a duplicate that only
    // removeClip cleaned up correctly, and that would otherwise stay frozen at
    // its pre-edit extent and clip the sum there.
    for( SLink *lk : childLinks() ) {
        if( !lk || !lk->hasStartTime() ) continue;
        offset_t startTime = lk->getStartTime();
        // Blocking read — same stale try-lock hazard as trackChildWasAdded.
        length_t duration = lk->getSObject().hasDuration() ? lk->getSObject().getDurationBlocking() : 0;
        // Create a callback that returns the component dynamically
        auto getComponentFn = [lk]() { return lk->getRootComponent(); };
        // Inv-1: single resolver — component + slip-folded position from ONE
        // clip snapshot, so the freeze can't straddle a lazy reader build.
        auto resolveFn = [lk]( offset_t off ) {
            return lk->getSObject().resolveClip( off );
        };
        for( int i=oldMixerCount; i<nBusses; ++i ) {
            if( !cpTrackMixers_[i] ) continue;
            cpTrackMixers_[i]->insertClip(lk, startTime, duration, getComponentFn, resolveFn);
        }
        // ...and give those entries the same live wiring trackChildWasAdded
        // makes, or a later move/resize of the clip would never reach the mixers
        // created here: the clip would stay at the extent it had at bus-growth
        // time. UniqueConnection because a child adopted the normal way is
        // already connected.
        QObject::connect( lk, SIGNAL( startTimeChanged( offset_t ) ),
                          this, SLOT( trackChildWasMoved( offset_t ) ),
                          Qt::UniqueConnection );
        QObject::connect( &(lk->getSObject()), SIGNAL( durationChanged( length_t ) ),
                          this, SLOT( trackChildDurationChanged( length_t ) ),
                          Qt::UniqueConnection );
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
    connectPluginChain( cpPluginChain_ );

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

// Signals + the reference + the component provider, in one place so the
// constructor path and the adoption path cannot drift apart (a chain wired by
// only one of them is a chain whose slots never reach the DSP).
void STrack::connectPluginChain( SPluginChain *chain )
{
    if( !chain ) return;

    QObject::connect( chain, SIGNAL( slotInserted( int, SPluginSlot & ) ),
                      this, SLOT( onPluginSlotInserted( int, SPluginSlot & ) ) );
    QObject::connect( chain, SIGNAL( slotRemoved( int, SPluginSlot & ) ),
                      this, SLOT( onPluginSlotRemoved( int, SPluginSlot & ) ) );
    QObject::connect( chain, SIGNAL( slotsReordered( int, int ) ),
                      this, SLOT( onPluginSlotsReordered( int, int ) ) );

    // Bus 0's DSP chain answers SPluginChain::getRootComponent(). Captured by
    // `this` and read lazily, so it follows a later setNBusses().
    chain->setComponentProvider( [this]() -> std::shared_ptr<twComponent> {
        if( cpPluginChains_.empty() || !cpPluginChains_[0] ) return nullptr;
        return std::static_pointer_cast<twComponent>( cpPluginChains_[0] );
    } );

    // See the member's declaration: without a reference of our own, an adopted
    // chain dies with the loader's temporary handle link.
    cpPluginChainRef_ = new SLink( *chain, nullptr );
}

// Proposal 08 M4: take over a chain that came out of a project file.
void STrack::adoptPluginChain( SPluginChain *chain )
{
    if( !chain || chain == cpPluginChain_ ) return;

    SPluginChain *old = cpPluginChain_;
    if( old ) {
        QObject::disconnect( old, nullptr, this, nullptr );
        old->setComponentProvider( nullptr );
    }
    // Dropping our reference is what retires the constructor's empty chain: its
    // refcount reaches zero and SObject::removeRef() posts the deleteLater. It
    // must go BEFORE the new link is made, or a save between the two would write
    // both chains and the track would name only one.
    delete cpPluginChainRef_;
    cpPluginChainRef_ = nullptr;

    cpPluginChain_ = chain;
    connectPluginChain( chain );

    // The loaded slots have never been near the DSP: their SLinks were parented
    // to the chain while it had no owner, so slotInserted was emitted into
    // nothing. Do here exactly what onPluginSlotInserted does, for every slot in
    // order — bus count FIRST (it selects the channel-mismatch mapping), then one
    // tap per bus appended to that bus's twPluginChain in slot order.
    const int nSlots = chain->getSlotCount();
    for( int s = 0; s < nSlots; ++s ) {
        SPluginSlot *slot = chain->getSlotAt( s );
        if( !slot ) continue;
        // The loaded slots never emitted slotInserted at anyone, so the
        // audioInvalidated wiring onPluginSlotInserted normally makes has to be
        // made here too — otherwise a bypass or parameter edit on a LOADED
        // project would be inaudible while the same edit on a freshly inserted
        // plugin worked.
        QObject::connect( slot, SIGNAL( audioInvalidated() ),
                          this, SLOT( onPluginSlotAudioInvalidated() ),
                          Qt::UniqueConnection );
        slot->setBusCount( nBusses_ );
    }
    for( int i = 0; i < nBusses_; ++i ) {
        if( !cpPluginChains_[i] ) continue;
        for( int s = 0; s < nSlots; ++s ) {
            SPluginSlot *slot = chain->getSlotAt( s );
            if( !slot ) continue;
            if( auto tap = slot->getInsertForBus( i ) )
                cpPluginChains_[i]->addPlugin( tap );
        }
    }
    invalidateRenderPath();
}

STrack::~STrack()
{
    DTOR_DEL( inlineRenderer_ );
    // Our reference to the plugin chain (see the member's declaration). The chain
    // object itself is a Qt child of the project and is destroyed with it.
    delete cpPluginChainRef_;
    cpPluginChainRef_ = nullptr;
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

    // Adopt our serialized plugin chain — DEFERRED (proposal 08 M4).
    //
    // It cannot happen here, and it cannot happen in readPostChildrenAttributes
    // either: pluginChainId is a plain attribute, so the loader's dependency
    // ordering (which only looks at <SLink objectId> children) gives no guarantee
    // that the <SPluginChain> element has been instantiated by the time this
    // track is built — with the chain listed after the track in the file, it
    // certainly has not. deferResolve runs the lookup at the end of
    // createObjects(), when the dictionary is complete.
    const QString chainId = element.attribute( "pluginChainId" );
    if( !chainId.isEmpty() ) {
        SProjectLoader *loader = &projectLoader;
        projectLoader.deferResolve( [loader, track, chainId]() {
            SLink *chainLink = loader->getObjectDictionary().value( chainId );
            if( !chainLink ) {
                qWarning() << "STrack: plugin chain" << chainId
                           << "not found in the project; keeping the empty one";
                return;
            }
            SPluginChain *chain =
                dynamic_cast<SPluginChain *>( &chainLink->getSObject() );
            if( !chain ) {
                qWarning() << "STrack: object" << chainId << "is not an SPluginChain";
                return;
            }
            track->adoptPluginChain( chain );
        } );
    }

    return new SLink( *track );
}

void STrack::onPluginSlotInserted( int index, SPluginSlot &slot )
{
    // BEFORE the bus guard, and UniqueConnection because a track whose bus count
    // grows re-runs this path: the slot has no way to invalidate the pages above
    // itself (proposal 08 M5 — see SPluginSlot::audioInvalidated()), so this
    // connection is what makes a bypass toggle and a parameter edit audible.
    QObject::connect( &slot, SIGNAL( audioInvalidated() ),
                      this, SLOT( onPluginSlotAudioInvalidated() ),
                      Qt::UniqueConnection );

    // Sync the model change to all DSP plugin chains
    // Pre-allocate inserts for all buses to ensure they're fully initialized
    // before the audio thread accesses them
    if( nBusses_ > 0 ) {
        // Tell the slot the bus count FIRST: it is what selects the
        // channel-mismatch mapping (proposal 08 §Layer 3), so deriving it once
        // here avoids re-instantiating the plugin per bus as the taps appear.
        slot.setBusCount( nBusses_ );

        // Then ensure all taps exist and are fully initialized
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
    // Remove by IDENTITY, never by index. `index` is a position in the model's
    // childOrder_; twPluginChain::plugins_ is a separate vector, and the two
    // agree only as long as every structural change is mirrored into both. When
    // they disagreed, removePlugin(index) erased a DIFFERENT insert than the one
    // being deleted: the model dropped the right slot, the audio path dropped
    // the wrong one, and nothing reported it. Identity cannot miss.
    //
    // peekInsertForBus (not getInsertForBus) because this is a teardown path —
    // it must never INSTANTIATE a plugin just to look up what to erase.
    (void)index;
    for( int i = 0; i < nBusses_; ++i ) {
        if( cpPluginChains_[i] ) {
            cpPluginChains_[i]->removePlugin( slot.peekInsertForBus( i ) );
        }
    }
    invalidateRenderPath();
}

void STrack::onPluginSlotAudioInvalidated()
{
    // Exactly what the insert/remove/reorder handlers do: bumpRenderChainEpoch()
    // on us (every bus's twTrackMix + twPluginChain + the rewire) and on every
    // container above us, via the root's walk.
    invalidateRenderPath();
}

void STrack::onPluginSlotsReordered( int fromIndex, int toIndex )
{
    // Make the SAME move in plugins_, do not merely re-wire. rebuildWiring()
    // alone wires plugins_ in ITS existing order, so the reorder was inaudible
    // AND it left plugins_ permanently out of step with the model — which is
    // what made a later removal target the wrong insert.
    for( int i = 0; i < nBusses_; ++i ) {
        if( cpPluginChains_[i] ) {
            cpPluginChains_[i]->reorderPlugin( fromIndex, toIndex );
        }
    }
    invalidateRenderPath();
}

// Mute belongs to the mixer CHANNEL, not to the track: it is enforced by
// whoever sums this track, never inside our own output. So this deliberately
// does NOT touch our own twTrackMixes — our rendered pages stay valid and keep
// carrying our material, which is what lets an asset window a muted track
// (SCut::buildCapture_ freezes our component directly; nobody sums it there).
//
// The two summing parents both react on their own:
//   - SStdMixer   -> trackMuteSoloChanged() -> reconnectTracksToMixer(), which
//                    nulls our input plug (and already did this for solo).
//   - STrack      -> childTrackMuteChanged() below, which mutes our clip entry.
void STrack::onTrackMuteChanged( bool /*muted*/ )
{
}

// A child TRACK of ours (a folder lane) changed its mute. We are the summing
// parent here, so we enforce it — the root mixer's null-the-input-plug trick
// cannot reach a nested track. Mute is per-lane (it changes nobody else's
// audibility), so re-applying our own children is enough.
void STrack::childTrackMuteChanged( bool /*muted*/ )
{
    applyChildTrackAudibility();
}

// A lane at or below one of our child tracks changed its solo flag. Solo is
// GLOBAL — it re-decides the audibility of every lane in the project, including
// our siblings and our parent's siblings — so this must not be resolved here.
// Relay it up; the root mixer answers with one whole-tree re-application
// (SStdMixer::applyAudibility), which calls back into
// applyChildTrackAudibility() on us.
void STrack::childTrackSoloChanged()
{
    emit subtreeSoloChanged();
}

// Enforce the shared audibility rule on our child TRACKS (folder lanes), and
// recurse. A lane's clip entry is muted exactly when the lane is not audible;
// twTrackMix::setClipMuted no-ops (and reports an empty range) when the flag is
// already right, so this is idempotent and costs nothing when nothing changed.
void STrack::applyChildTrackAudibility()
{
    SObject *root = splacements::rootContainer( getProjectSafe() );
    const bool anySolo = ssolo::anySoloInTree( root );

    twEditRange affected;
    for( SLink *lk : childLinks() ) {
        if( !lk ) continue;
        STrack *child = dynamic_cast<STrack*>( &lk->getSObject() );
        if( !child ) continue;
        const bool audible = ssolo::isLaneAudible( root, child, anySolo );
        for( int i=0; i<nBusses_; ++i ) {
            if( cpTrackMixers_[i] ) {
                twEditRange r = cpTrackMixers_[i]->setClipMuted( lk, !audible );
                affected.unite( r.start, r.end );
            }
        }
        child->applyChildTrackAudibility();   // nested folders
    }

    // Only the lanes that actually flipped gained or lost material — and the
    // chain above us has to be staled the same way, or the summed pages we
    // already froze keep serving the pre-edit mix (AC6 / the mute precedent).
    if( !affected.empty() ) {
        invalidateRenderPathRange( (offset_t) affected.start,
                                   (offset_t) affected.end );
    }
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
          STrack::instantiateFromDomElement,
          // A CONTAINER of clips and nested lanes: one unloadable clip costs
          // its own link, never the track (proposal 36 D8a).
          SElementKind::Container ), true );
