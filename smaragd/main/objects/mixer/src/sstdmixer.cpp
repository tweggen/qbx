
#include <iostream>
#include <stdlib.h>
#include <vector>

#include <QDebug>

#include "tw/mix/twrewire.h"
#include "tw/mix/twmixer.h"

#include "app/model/sproject.h"
#include "app/objects/mixer/sstdmixer.h"
#include "app/objects/track/strack.h"   // upcast of the selected track
#include "app/model/sdetaileditors.h"
#include "app/model/ssolorules.h"       // the one mute/solo audibility rule
#include "app/model/sobjectpath.h"     // the system-lane sentinels (proposal 45 D9)
#include "app/persistence/sprojectloader.h"
#include "app/model/sappcontext.h"
#include "tw/schedule/capture_aspects.h"  // Preview/Playback/... bits

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
    // Created via the registered factory (app/timeline registers
    // SStdMixerView) — the model constructs no view types (Phase 6).
    return sdetaileditors::create( *this, parent );
}

QWidget *SStdMixer::getInlineEditWidget( QWidget * )
{
    return NULL;
}

SObjectRenderer *SStdMixer::getInlineRenderer()
{
    return NULL;
}

SObject *SStdMixer::activeLane() const
{
    return getSelectedTrack();
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

std::shared_ptr<twComponent> SStdMixer::getRootComponent()
{
    // (The "FIXME: Generate a channel reassignment." that stood here since the
    // beginning is discharged by proposal 36 B4: twRewire IS the channel
    // reassignment now — output channel c is input channel map[c], identity by
    // default — and this rewire is the graph ROOT that the render session and
    // the playback readahead pull. Nothing here has to reassign anything; the
    // component below does, on request.)
    return std::static_pointer_cast<twComponent>(cpRewire_);
}

// Proposal 36 B4. ONE bus, N channels: the bus count stays 1 forever and the
// width is a separate number, because a bus and a channel were never the same
// thing and conflating them is what dropped every track's second channel here.
void SStdMixer::setChannels( int n )
{
    if( n < 1 ) n = 1;
    if( n == channels_ ) return;
    channels_ = n;

    for( int bus = 0; bus < nBusses_; ++bus ) {
        if( cpMixers_[bus] ) cpMixers_[bus]->setChannels( (idx_t) n );
    }
    if( cpRewire_ ) cpRewire_->setChannels( (idx_t) n );

    // Pages of the old width read as misses (§4.5); bump so the master
    // re-freezes instead of playing silence until some other edit does it.
    bumpRenderChainEpoch();
}

void SStdMixer::bumpRenderChainEpoch()
{
    // The summed mix is cached at TWO levels: the per-bus twMixer (sums the
    // track outputs) and the rewire behind it (the engine's synthOutput_).
    for( int bus=0; bus<nBusses_; ++bus ) {
        if( cpMixers_[bus] )
            cpMixers_[bus]->bumpContentEpoch();
    }
    if( cpRewire_ )
        cpRewire_->bumpContentEpoch();
}

// Range-scoped variant (proposal 18 Phase 5); positions are shared across
// the summing chain.
void SStdMixer::bumpRenderChainEpochRange( offset_t start, offset_t end )
{
    for( int bus=0; bus<nBusses_; ++bus ) {
        if( cpMixers_[bus] )
            cpMixers_[bus]->invalidatePagesInRange(start, end);
    }
    if( cpRewire_ )
        cpRewire_->invalidatePagesInRange(start, end);
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
    int nTracks = childCount();
    // ONE whole-tree solo scan for the whole pass (see ssolorules.h).
    const bool solo = ssolo::anySoloInTree( this );
    // For all busses.
    for( int bus=0; bus<nBusses_; bus++ ) {
        std::shared_ptr<twMixer> mix = cpMixers_[bus];
        if( !mix ) continue;
        // Ensure the given number of inputs.
        mix->setNInputs( nTracks );
        for( int channel=0; channel<nTracks; channel++ ) {
            SLink *lk = childAt( channel );
            // The shared audibility rule (ssolo::isLaneAudible): a folder that
            // CONTAINS a soloed lane has to stay wired, or the nested solo would
            // be silenced by its own parent. Inaudible tracks get a NULL input so
            // their DSP is not pulled at all (processing AND output disabled).
            //
            // THE LIVE-OWNED PREDICATE (proposal 21 L1b, design D3) is a
            // SECOND, SEPARATE term, deliberately not folded into
            // ssolo::isLaneAudible: a live-owned track is excluded from the
            // FROZEN SUM (the pump renders it and the RT adds the ring), but
            // it is still audible in every other sense - its events still
            // reach a folder instrument's feed and its meters still light.
            // Folding the two would darken both. A top-level closure member
            // is always the TOPMOST one by construction, which is why the
            // mixer's rule is simply "in the closure => null the plug".
            bool audible = false;
            bool liveOwned = false;
            if( lk ) {
                SObject &so = lk->getSObject();
                audible = ssolo::isLaneAudible( this, &so, solo );
                if( STrack *t = dynamic_cast<STrack *>( &so ) )
                    liveOwned = t->isLiveOwnedLane();
            }
            if( !lk || !audible || liveOwned ) {
                mix->setInput( channel, NULL );
                mix->setInputLevel( channel, 0 );
            } else {
                std::shared_ptr<twComponent> root = lk->getRootComponent();
                mix->setInput( channel, root->linkOutput( bus ) );
                // Unity (0 dB): the track applies its own gain intrinsically
                // (see twTrackMix::calcOutputTo), so the mixer just sums.
                mix->setInputLevel( channel, 0.0 );
            }
        }
    }
}

/**
 * True if at least one lane ANYWHERE in the project has its solo flag set.
 *
 * This used to iterate the mixer's direct children only, which made a solo on a
 * lane nested inside a folder track invisible to the routing rule (and to the
 * meters, which ask the same question) — one of the three reasons nested solo
 * did nothing at all.
 */
bool SStdMixer::anyTrackSoloed() const
{
    return ssolo::anySoloInTree( const_cast<SStdMixer *>( this ) );
}

/**
 * Re-apply the audibility rule over the WHOLE lane tree.
 *
 * Solo is global, so one flag flip anywhere changes what every summing
 * container must do — and there are two kinds of summing container, enforcing
 * audibility in two different ways:
 *
 *   - this mixer, for its direct children: null the input plug;
 *   - a folder STrack, for its nested lanes: twTrackMix::setClipMuted.
 *
 * Driving both from one top-down pass is what keeps them from disagreeing.
 */
void SStdMixer::applyAudibility()
{
    reconnectTracksToMixer();
    for( SLink *lk : childLinks() ) {
        if( !lk ) continue;
        if( STrack *t = dynamic_cast<STrack *>( &lk->getSObject() ) )
            t->applyChildTrackAudibility();
    }
}

/**
 * A track's mute/solo changed. Solo is global (it silences every non-soloed
 * track), so just re-evaluate the whole routing.
 */
void SStdMixer::trackMuteSoloChanged()
{
    applyAudibility();
    // Rewiring only changes what FUTURE freezes produce. Pages already frozen
    // here (and downstream at the rewire) still contain the track, so playback
    // readahead and the next render would go on serving it — you mute a track
    // and keep hearing it until those pages age out. Drop them.
    //
    // Mute used to get this for free: it was baked into the track's own output,
    // and STrack::onTrackMuteChanged called invalidateRenderPath(). Now that
    // mute is a channel property enforced here, the invalidation belongs here
    // too. SOLO has always come through this slot with no invalidation at all,
    // so it carried the same staleness — this fixes both.
    invalidateRenderPath();
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
    // no change?
    if( n<0 ) return -2;
    if( n==nBusses_ ) return 0;
    int nTracks = childCount();

    qWarning( "SStdMixer::setNBusses( %d ): called.\n", n );

    // First check, if still input tracks are connected to the
    // busses that should vanish.
    if( n<nBusses_ ) {
        for( int i=n; i<nBusses_; i++ ) {
            int still = cpMixers_[i]->getInputsSet();
            if( still ) {
                qWarning( "SStdMixer::setNBusses(): Unable to remove channel %d: "
                          "Still %d objects connected.\n", i, still );
                return -1;
            }
        }
    }
    // Takeover contains the the number of busses we can takeover from the current installation.
    int nTakeOver = n;
    if( nBusses_<nTakeOver ) nTakeOver = nBusses_;
    // Use the old mixers

    // If we have to free old unused ones, do it.
    // In the same shot, we remove them from our rewirer.
    for( int i=nTakeOver; i<nBusses_; i++ ) {
        cpRewire_->setInput( i, NULL );
        qWarning( "SStdMixer::setNBusses( %d ): Deleting old mixer #%d.\n", n, i );
        cpMixers_[i].reset();
    }
    // Set the rewirer to the proper number of channels.
    cpRewire_->setNPlugs( n );

    // Resize the mixer vector to exactly n slots. The old raw-pointer code
    // allocated a fresh array of size n on every call; the vector port must
    // grow (or shrink) it explicitly, otherwise the alloc loop below writes
    // past the end via operator[] — undefined behavior. This bites on the very
    // first call from the constructor, where nBusses_==0 and cpMixers_ is
    // still empty. Existing takeover slots [0,nTakeOver) are preserved; freed
    // tail slots were already reset above, so shrinking just drops nulls.
    cpMixers_.resize( n );

    // If we have to alloc new ones, do it.
    for( int i=nTakeOver; i<n; i++ ) {
        qWarning( "SStdMixer::setNBusses( %d ): Creating new mixer #%d.\n", n, i );
        int nc = nTracks;
        if( nc<1 ) nc = 1;
        std::shared_ptr<twMixer> mix = std::make_shared<twMixer>( *(SAppContext::get().get303aEnvironment()), nc );
        mix->init();
        mix->setChannels( (idx_t) channels_ );
        cpRewire_->setInput( i, mix->linkOutput( 0 ) );
        cpMixers_[i] = mix;
    }
    nBusses_ = n;

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
    // A solo on a lane NESTED inside this track is equally our business: solo is
    // global, so the whole tree's routing has to be re-evaluated. A nested lane
    // is not our child and reparenting explicitly drops track->mixer connections
    // (SReparentTrackAction), so it cannot reach us directly — every folder
    // forwards its subtree's solo changes up as this signal.
    QObject::connect(
        (QObject*)&trk, SIGNAL( subtreeSoloChanged() ),
        this, SLOT( trackMuteSoloChanged() ) );
    // Construction parents the link to us, which appends it (childEvent keeps
    // childOrder_ in sync). Position it afterwards with reorderTrack().
    SLink *lk = new SLink( (SObject&)trk, this );
    (void) lk;
    int newIndex = childCount() - 1;   // actual landing index (append)
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
    // Drop our reference to the master lane FIRST. The lane is a Qt child of
    // SProject, so this is what stops a removed arrangement leaving an orphan
    // lane behind that would serialize forever: the refcount reaches zero and
    // SObject::removeRef() posts the deleteLater.
    delete masterLaneRef_;
    masterLaneRef_ = nullptr;
    masterLane_    = nullptr;

    cpMixers_.resize(0);
    cpRewire_.reset();
}

QList<SLink *> SStdMixer::ownedRefLinks() const
{
    // See the declaration: the master-lane reference is an owned link that is
    // NOT a child link, so it has to be published here to be part of the
    // reference graph at all.
    QList<SLink *> out;
    if( masterLaneRef_ ) out.append( masterLaneRef_ );
    return out;
}

SObject *SStdMixer::systemLaneAt( int sentinel ) const
{
    // -1 is the master. The send sentinels (-2, -3, ...) are RESERVED and
    // answer null until proposal 45 M7 builds them, which is also what stops
    // strackpath::findPathRec's descending scan before it runs off the end.
    if( sentinel == strackpath::SPATH_MASTER )
        return static_cast<SObject *>( masterLane_ );
    return nullptr;
}

int SStdMixer::systemLaneSentinelOf( const SObject *lane ) const
{
    if( lane && lane == static_cast<const SObject *>( masterLane_ ) )
        return strackpath::SPATH_MASTER;
    return 0;
}

void SStdMixer::adoptMasterLane( STrack *lane )
{
    if( !lane || lane == masterLane_ ) return;

    // Dropping our reference is what retires the constructor's fresh lane: its
    // refcount reaches zero and SObject::removeRef() posts the deleteLater. It
    // must go BEFORE the new link is made, or a save between the two would
    // write both lanes and the mixer would name only one -- the exact ordering
    // STrack::adoptPluginChain() spells out for the same reason.
    delete masterLaneRef_;
    masterLaneRef_ = nullptr;

    masterLane_    = lane;
    masterLaneRef_ = new SLink( *lane, nullptr );
}

SStdMixer::SStdMixer( SProject *project )
    : SObject( project ),
      nBusses_( 0 ),
      lastDuration_( 1 ),
      lastDurationValid_( true )
{
    // WIDTH COMES FROM THE PROJECT (proposal 36 B4) and is read BEFORE the
    // components exist, so the first twMixer and the rewire are built at the
    // right width rather than widened a moment later.
    channels_ = project ? project->channels() : 2;
    cpRewire_ = std::make_shared<twRewire>( *(SAppContext::get().get303aEnvironment()) );
    cpRewire_->init();
    cpRewire_->setChannels( (idx_t) channels_ );
    cpRewire_->setNPlugs( 1 );   // a wide rewire is single-plug
    if( project ) {
        QObject::connect( project, SIGNAL( channelsChanged( int ) ),
                          this, SLOT( setChannels( int ) ),
                          Qt::UniqueConnection );
    }
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

    // THE MASTER LANE (proposal 45 D1/D2). Minted here rather than lazily so
    // masterLane() is never null and no caller needs a "what if there is no
    // master" branch; every arrangement root created at runtime
    // (create-arrangement, extract-arrangement) therefore gets one for free.
    //
    // Constructed HIDDEN (D8): it is available, not in the way. Hiding is a
    // VIEW property and never an audio one -- a hidden master lane is fully in
    // the signal path.
    masterLane_ = new STrack( project );
    masterLane_->setSystemRole( SSystemRole::Master );
    masterLane_->setSName( QStringLiteral( "Master" ) );
    masterLane_->setHidden( true );
    // Our own reference. Without one the lane, which hangs off no child link,
    // has a refcount of zero and is deleted the moment anything looks at it.
    masterLaneRef_ = new SLink( *masterLane_, nullptr );
}

// --- track selection --------------------------------------------------------
//
// One set, one primary. Every mutator funnels through setSelectedTracks() so
// there is a single place that decides which signals fire, and a single place
// that drops dead QPointers — a selection that outlived one of its tracks (a
// removed-then-discarded track) must never hand a dangling pointer out.

STrack *SStdMixer::getSelectedTrack() const
{
    return selectedTrack_.data();
}

QList<STrack *> SStdMixer::getSelectedTracks() const
{
    QList<STrack *> out;
    for( const QPointer<STrack> &p : selectedTracks_ ) {
        if( p ) out.append( p.data() );
    }
    return out;
}

int SStdMixer::nSelectedTracks() const
{
    return getSelectedTracks().size();
}

bool SStdMixer::isTrackSelected( STrack *track ) const
{
    if( !track ) return false;
    for( const QPointer<STrack> &p : selectedTracks_ ) {
        if( p.data() == track ) return true;
    }
    return false;
}

void SStdMixer::setSelectedTrack( STrack *track )
{
    QList<STrack *> one;
    if( track ) one.append( track );
    setSelectedTracks( one, track );
}

void SStdMixer::setSelectedTracks( const QList<STrack *> &tracks, STrack *primary )
{
    // Normalize: drop nulls and duplicates, keeping the caller's order.
    QList<QPointer<STrack> > next;
    for( STrack *t : tracks ) {
        if( !t ) continue;
        bool dup = false;
        for( const QPointer<STrack> &p : next ) if( p.data() == t ) { dup = true; break; }
        if( !dup ) next.append( QPointer<STrack>( t ) );
    }

    STrack *nextPrimary = primary;
    bool primaryIsMember = false;
    for( const QPointer<STrack> &p : next ) if( p.data() == nextPrimary ) { primaryIsMember = true; break; }
    if( !primaryIsMember ) {
        nextPrimary = next.isEmpty() ? nullptr : next.last().data();
    }

    // Compare against the CURRENT live set (dead entries count as gone), so a
    // selection that lost a track still reports a change once.
    const QList<STrack *> before = getSelectedTracks();
    QList<STrack *> after;
    for( const QPointer<STrack> &p : next ) after.append( p.data() );

    const bool setChanged     = ( before != after );
    const bool primaryChanged = ( selectedTrack_.data() != nextPrimary );
    if( !setChanged && !primaryChanged ) return;

    selectedTracks_ = next;
    selectedTrack_  = nextPrimary;

    // Set first, then primary: a view that repaints on the set signal and a
    // panel that rebinds on the primary signal both see the settled state.
    if( setChanged )     emit selectedTracksChanged();
    if( primaryChanged ) emit selectedTrackChanged( nextPrimary );
}

void SStdMixer::toggleTrackSelection( STrack *track )
{
    if( !track ) return;
    QList<STrack *> next = getSelectedTracks();
    if( next.removeAll( track ) > 0 ) {
        // Left the selection: the primary passes to whoever is left.
        setSelectedTracks( next, next.isEmpty() ? nullptr : next.last() );
    } else {
        next.append( track );
        setSelectedTracks( next, track );
    }
}

int SStdMixer::serializeSelfAttributes( QTextStream &o )
{
    // The master lane (proposal 45 D2). A plain attribute, not an <SLink>
    // child: SObject::childEvent treats every child link as a track placement
    // and the loader's dependency ordering is built on child links -- which is
    // exactly why the load side has to defer. The lane object itself is a Qt
    // child of SProject, so SProject::serialize()'s own child loop writes it.
    if( masterLane_ )
        o << " masterLaneId='"
          << reinterpret_cast<std::uintptr_t>( (SObject *) masterLane_ ) << "'";
    SObject::serializeSelfAttributes( o );
    return 0;
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
                
    // A named ARRANGEMENT root re-registers itself HERE, during instantiation
    // (proposal 09 D3). Not from a deferResolve, and the difference is the
    // whole point: registerArrangement() takes a REFERENCE, and this root
    // hangs off no SLink in the master tree, so anything later than "the
    // moment it exists" races ~SProjectLoader dropping the handle links --
    // after which the refcount is zero and the object is gone (by
    // deleteLater(), so it does not even look like a crash).
    //
    // The attribute is absent on every mixer written before this, and on the
    // master root always, so this is inert for existing files.
    const QString arrName = element.attribute( "arrangementName" );
    if( !arrName.isEmpty() )
        projectLoader.getProject().registerArrangement( arrName, mixer );

    // Adopt our serialized master lane -- DEFERRED (proposal 45 D2), for the
    // reason STrack::instantiateFromDomElement spells out for pluginChainId:
    // masterLaneId is a plain attribute, so the loader's dependency ordering
    // (which only looks at <SLink objectId> children) gives no guarantee the
    // lane's own <STrack> element has been instantiated by the time we are
    // built. deferResolve runs the lookup when the dictionary is complete.
    //
    // A REFERENCE THAT DOES NOT NAME A MASTER LANE IS REFUSED, not adopted
    // (AC1.5). An older build re-saving this file drops systemRole= along with
    // masterLaneId=, so a file can legitimately come back naming a track that
    // is now an ordinary one; adopting it would make a user track the master
    // -- silently accepting clips, arming, and being summed twice. Keeping the
    // constructor's own lane is the safe answer, and it is announced.
    const QString laneId = element.attribute( "masterLaneId" );
    if( !laneId.isEmpty() ) {
        SProjectLoader *loader = &projectLoader;
        projectLoader.deferResolve( [loader, mixer, laneId]() {
            SLink *laneLink = loader->getObjectDictionary().value( laneId );
            if( !laneLink ) {
                qWarning() << "SStdMixer: master lane" << laneId
                           << "not found in the project; keeping the fresh one";
                return;
            }
            STrack *lane = dynamic_cast<STrack *>( &laneLink->getSObject() );
            if( !lane ) {
                qWarning() << "SStdMixer: masterLaneId" << laneId
                           << "does not name a track; keeping the fresh one";
                return;
            }
            if( lane->systemRole() != SSystemRole::Master ) {
                qWarning() << "SStdMixer: masterLaneId" << laneId
                           << "names a track whose systemRole is not master"
                           << "(an older build may have re-saved this project);"
                           << "keeping the fresh one";
                return;
            }
            mixer->adoptMasterLane( lane );
        } );
    }

    // Construct with parent=NULL, then setParent (slink.h rule): the parent's
    // childEvent must never see a half-constructed SLink.
    SLink *mixerLink = new SLink( *mixer, NULL );
    if( parent ) mixerLink->setParent( parent );
    return mixerLink;
}

// Phase 5e: Page cache implementation

// Self-registration with the project loader (proposal 14, Phase 5): the
// persistence module names no concrete types; each slice registers its own
// element name. Relies on the app being an OBJECT library (no TU elision).
static const bool s_registered_sstdmixer =
    ( SProjectLoader::registerSObjectClass( "SStdMixer",
          SStdMixer::instantiateFromDomElement,
          // A CONTAINER of tracks: one unloadable track costs its own link,
          // never the whole mixer — which IS the project (proposal 37 D8a).
          SElementKind::Container ), true );
