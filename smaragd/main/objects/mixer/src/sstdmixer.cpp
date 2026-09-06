
#include <iostream>
#include <stdlib.h>
#include <vector>

#include <QDebug>
#include <QHash>
#include <QSet>

#include "tw/core/twlog.h"

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
    // The summed mix is cached at THREE levels since proposal 45 M2: the
    // per-bus twMixer (sums the track outputs), the master lane's chain and
    // gain stage, and the rewire behind them (the engine's synthOutput_).
    for( int bus=0; bus<nBusses_; ++bus ) {
        if( cpMixers_[bus] )
            cpMixers_[bus]->bumpContentEpoch();
    }
    bumpMasterChainEpoch();
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
    bumpMasterChainEpochRange( start, end );
    if( cpRewire_ )
        cpRewire_->invalidatePagesInRange(start, end);
}

// --- D3a: invalidation must travel THROUGH the master chain ------------------
//
// THIS IS THE COMMONEST PATH IN THE APP AND IT IS SILENT WHEN WRONG. Every
// user has an EMPTY master chain and edits clips; after M2 put the master
// lane's twPluginChain and twGainStage between the bus sum and the rewire,
// each carries its own page cache and neither was bumped by anybody on an
// ordinary edit. The rewire re-freezes, fetchInputPage serves the gain
// stage's still-valid stale page, and the edit is INAUDIBLE -- no error, no
// log line, just the old audio.
//
// Measured, not predicted: with the wiring in and this missing,
// mc_golden_stereo's own negative control flipped. Its `set-track-mute` +
// re-render produced a file BYTE-IDENTICAL to the unmuted golden, so the
// assertion that exists to prove the comparison can fail reported "applied
// but expectReject was set". A mute stopped being audible.
//
// It is a DIFFERENT edge from D11's "invalidation FROM the master" and
// neither subsumes the other: this one carries an ordinary track edit PAST
// the master, that one carries a master edit DOWN to the rewire.
// STrack::bumpRenderChainEpoch already bumps its trackmix, chain, gain stage
// and rewire -- exactly the set that needs bumping, and the two inert ones cost
// nothing. Calling it is also what keeps this module free of tw/plugins.
void SStdMixer::bumpMasterChainEpoch()
{
    if( masterLane_ ) masterLane_->bumpRenderChainEpoch();
}

void SStdMixer::bumpMasterChainEpochRange( offset_t start, offset_t end )
{
    if( masterLane_ ) masterLane_->bumpRenderChainEpochRange( start, end );
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

    // THE SEND LANES ARE WIRED BY THIS PASS, NOT BESIDE IT (proposal 45 M7,
    // trap T3). This is the whole of AC7.4 and it is the one thing about send
    // lanes that had to be got right rather than merely added.
    //
    // This function sets the input count FROM THE TRACK COUNT and rewires
    // EVERY input, and it runs on every audibility, solo, mute and arm change.
    // So a send lane wired into a spare input from anywhere else -- at
    // creation, at load, from a verb -- is silently CLOBBERED by the next
    // solo toggle, and the symptom is a send that worked until the user
    // pressed a button somewhere else entirely. Reserving the inputs AFTER the
    // tracks and filling them in the same loop is what makes the wiring
    // idempotent under repetition, which is what "survives a rewire" means.
    const QList<STrack *> sends = sendLanes_;

    // For all busses.
    for( int bus=0; bus<nBusses_; bus++ ) {
        std::shared_ptr<twMixer> mix = cpMixers_[bus];
        if( !mix ) continue;
        // Ensure the given number of inputs: the tracks, then the send lanes.
        mix->setNInputs( nTracks + sends.size() );
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

        // ...and the send lanes, into the inputs reserved above.
        for( int k = 0; k < sends.size(); ++k ) {
            const int channel = nTracks + k;
            STrack *send = sends[k];
            if( !send ) {
                mix->setInput( channel, NULL );
                mix->setInputLevel( channel, 0 );
                continue;
            }
            std::shared_ptr<twComponent> root = send->getRootComponent();
            mix->setInput( channel, root ? root->linkOutput( bus ) : NULL );
            // UNITY, and this is load-bearing rather than a copy of the line
            // above. twlive::checkMasterShape walks getNInputs() and refuses
            // the LINEAR master split the moment ANY input level is not 0 dB
            // (D4a rule 2), so a send wired at anything else would not merely
            // be mis-levelled -- it would silently drop live monitoring into
            // the Closure path for every armed track in the project, which is
            // the "monitoring dies for a new reason" D10 warns about. The
            // send's own level is its twGainStage, exactly as a track's is.
            mix->setInputLevel( channel, 0.0 );
            // MUTE IS NOT APPLIED HERE, unlike a track's. A track's mute is
            // STRUCTURAL -- this pass nulls its plug -- but M5/AC5.4 wired a
            // SYSTEM lane's mute to its twGainStage instead, because a system
            // lane has no summing parent to do it. Nulling the plug too would
            // be the same silence twice and would make the ramp unreachable.
            // SOLO is not applied either, and cannot be: ssolo::anySoloInTree
            // walks childLinks(), which a send lane is deliberately not in. It
            // is moot while nothing can feed a send (D10) and it belongs to
            // the routing milestone, not to this one.
        }
    }

    // ...and the send BUSES, in the same pass and from nowhere else (47 D4).
    rewireSendBuses();
}

/**
 * Proposal 47 M1. Fill every send lane's bus from the taps that address it.
 *
 * Index-parallel to sendLanes_ throughout: bus k belongs to lane k, which is
 * the lane the sentinel `-2 - k` addresses.
 */
int SStdMixer::sendBusInputCount( int k ) const
{
    if( k < 0 || k >= sendBuses_.size() || !sendBuses_[k] ) return -1;
    return (int) sendBuses_[k]->getNInputs();
}

bool SStdMixer::sendBusInputWired( int k, int i ) const
{
    if( k < 0 || k >= sendBuses_.size() || !sendBuses_[k] ) return false;
    if( i < 0 || i >= (int) sendBuses_[k]->getNInputs() ) return false;
    return sendBuses_[k]->getInputPlug( (idx_t) i ) != nullptr;
}

double SStdMixer::sendBusInputLevelDb( int k, int i ) const
{
    if( k < 0 || k >= sendBuses_.size() || !sendBuses_[k] ) return 0.0;
    if( i < 0 || i >= (int) sendBuses_[k]->getNInputs() ) return 0.0;
    return sendBuses_[k]->inputLevel( (idx_t) i );
}

namespace {

/// True when `from` reaches `to` by following the ACCEPTED send edges
/// (`fed[x]` = the lanes x feeds). Proposal 47 M3.
bool sendReaches( const QHash<SObject *, QList<SObject *> > &fed,
                  SObject *from, const SObject *to )
{
    QSet<const SObject *> seen;
    QList<SObject *> work;
    work.append( from );
    while( !work.isEmpty() ) {
        SObject *cur = work.takeLast();
        if( !cur || seen.contains( cur ) ) continue;
        seen.insert( cur );
        if( cur == to ) return true;
        for( SObject *nxt : fed.value( cur ) ) work.append( nxt );
    }
    return false;
}

}  // namespace

void SStdMixer::rewireSendBuses()
{
    // Keep the bus list the same length as the lane list. A lane removed from
    // the END takes its bus with it (remove-send-lane is restricted to the
    // last, 45/M7), and a lane adopted from a file gets one here rather than
    // in adoptSendLane -- D4 again: one place wires, so one place allocates.
    while( sendBuses_.size() > sendLanes_.size() ) sendBuses_.removeLast();
    while( sendBuses_.size() < sendLanes_.size() ) {
        // ONE input, not zero: twMixer refuses zero by contract (see the loop
        // below). It is left unwired until a tap addresses this lane.
        auto bus = std::make_shared<twMixer>(
            *( SAppContext::get().get303aEnvironment() ), 1 );
        bus->init();
        sendBuses_.append( bus );
    }
    if( sendLanes_.isEmpty() ) return;   // T8: nothing wired, nothing changes

    // ONE whole-tree solo scan for the whole pass, exactly as the master half
    // above takes one (ssolorules.h). ASKED, never re-spelled (47 D5): two
    // local copies of the direct-children-only rule are how the meter and the
    // ear came to disagree about a nested lane (timeline/CONTRACT inv. 10).
    const bool solo = ssolo::anySoloInTree( this );

    // Every object that may carry a tap: the user tracks, and the send lanes
    // themselves (send -> send is legal, subject to the verb's cycle walk).
    // The MASTER is deliberately absent -- its output is the sum that already
    // contains every send lane, which is why add-send refuses it by role.
    QList<SObject *> sources;
    for( SLink *lk : childLinks() )
        if( lk ) sources.append( &lk->getSObject() );
    for( STrack *lane : sendLanes_ )
        if( lane ) sources.append( static_cast<SObject *>( lane ) );

    // The edges accepted so far: fedLanes[x] is the set of lanes x feeds.
    // Built as the lanes are wired, in index order, so the cycle break below
    // is deterministic rather than dependent on edit order.
    QHash<SObject *, QList<SObject *> > fedLanes;

    for( int k = 0; k < sendLanes_.size(); ++k ) {
        STrack *lane = sendLanes_[k];
        std::shared_ptr<twMixer> bus = sendBuses_[k];
        if( !lane || !bus ) continue;

        bus->setChannels( (idx_t) channels_ );

        // The taps addressing THIS lane, in source order, so an input index is
        // stable for a given project rather than depending on the order edits
        // happened to arrive in.
        struct Feed { SObject *src; SSendTap tap; };
        QList<Feed> feeds;
        for( SObject *src : sources ) {
            if( src == static_cast<SObject *>( lane ) ) continue;  // no self
            const SSendTap *t = src->sendTap( lane->getSName() );
            if( !t || !t->enabled ) continue;

            // THE CYCLE BREAK, AND IT LIVES HERE RATHER THAN ONLY IN THE VERB
            // (proposal 47 M3). `add-send` refuses a cycle, but the LOADER
            // does not go through it: `<sends>` is read by
            // SObject::readSends(), so a hand-edited or foreign .qxp can
            // carry Alpha -> Beta -> Alpha and the verb never sees it.
            //
            // Accept an edge only when it does not close a cycle among the
            // edges ALREADY accepted, walking lanes in index order. That is
            // deterministic (lane order is file order) and it drops exactly
            // one edge of a cycle rather than both, which asking the full tap
            // graph instead of the accepted one would do.
            if( sendReaches( fedLanes, static_cast<SObject *>( lane ), src ) ) {
                TW_LOGW( "model",
                         "[SEND] cycle: the tap from '%s' into '%s' is DROPPED "
                         "-- '%s' already feeds '%s'. A send graph must be "
                         "acyclic; add-send refuses one, a project file can "
                         "still carry one.",
                         src->getSName().toUtf8().constData(),
                         lane->getSName().toUtf8().constData(),
                         lane->getSName().toUtf8().constData(),
                         src->getSName().toUtf8().constData() );
                continue;
            }
            fedLanes[src].append( static_cast<SObject *>( lane ) );
            feeds.append( { src, *t } );
        }

        // TWMIXER REFUSES ZERO INPUTS BY CONTRACT (`if( n<=0 ) return -2`), so
        // "no taps" is ONE UNWIRED input and never a request for none.
        // MEASURED with `setNInputs( 0 )`: the refusal went unhandled, the bus
        // kept the plug it already had, and removing the last tap left the send
        // sounding at the pre-removal level forever -- a render read 0.34671
        // where 0.115478 was due. The refusal is right (a mixer with no inputs
        // has no output to define); expressing the empty case is this caller's
        // job.
        const int nIn = feeds.isEmpty() ? 1 : feeds.size();
        bus->setNInputs( (idx_t) nIn );
        for( int i = 0; i < nIn; ++i ) {
            if( i >= feeds.size() ) {   // the unwired tail, and the empty case
                bus->setInput( (idx_t) i, NULL );
                bus->setInputLevel( (idx_t) i, 0 );
                continue;
            }
            STrack *srcTrack = dynamic_cast<STrack *>( feeds[i].src );
            bool contributes = srcTrack != nullptr;

            // The SAME audibility rule the master sum applies (D5). A muted or
            // solo-darkened source feeds nothing, and mute kills a PRE-fader
            // send too -- not derivable from the tap point, and therefore a
            // decision: every reference DAW silences sends on mute.
            if( contributes )
                contributes = ssolo::isLaneAudible( this, feeds[i].src, solo );

            // D9, DECIDED: A LIVE LANE DOES NOT FEED A SEND. A live-owned
            // track is excluded from the frozen sum and rendered by the pump
            // straight into the ring, so the gain-stage pages this tap would
            // read are not being produced at all. A SECOND term beside
            // isLaneAudible and never folded into it, exactly as the master
            // half keeps them apart (21 L1b): a live-owned track is still
            // audible in every other sense.
            if( contributes && srcTrack->isLiveOwnedLane() ) contributes = false;

            if( !contributes ) {
                bus->setInput( (idx_t) i, NULL );
                bus->setInputLevel( (idx_t) i, 0 );
                continue;
            }

            // ASKED OF THE TRACK, not resolved here: `app/objects/mixer` may
            // not name twPluginChain, which is the same division
            // wireAsMasterLane draws -- the mixer passes endpoints and owns the
            // wiring, the track decides what its own internals are. D3: PRE is
            // post-FX (the chain's output), POST is post-fader (the gain
            // stage's).
            std::shared_ptr<twComponent> tap =
                srcTrack->sendTapComponent( feeds[i].tap.preFader );
            bus->setInput( (idx_t) i, tap ? tap->linkOutput( 0 ) : NULL );
            // D7: the send LEVEL is the bus's own per-input level. No new DSP,
            // no new ramp, nothing new on the render path -- and unlike the
            // MASTER sum's inputs it may be non-unity freely, because
            // checkMasterShape inspects the master mixer and never this one.
            bus->setInputLevel( (idx_t) i, feeds[i].tap.levelDb );
        }

        // ...and the lane reads the bus instead of its (empty) clip mix (D1).
        lane->wireAsSendLane( bus );
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

void SStdMixer::sendRoutingChanged()
{
    reconnectTracksToMixer();

    // ...AND INVALIDATE, which is proposal 47 M2 and is not optional. Without
    // this line the gate fails: a `set-send` level change is wired correctly
    // and then INAUDIBLE, because the render serves pages frozen before the
    // edit. Proposal 45 measured the identical shape for the master mute,
    // where an epoch bump alone left the muted render byte-identical to the
    // unmuted one.
    //
    // ONE LINE, AND IT IS THE FULL WALK. Bumping the send lanes'
    // bumpRenderChainEpoch() and the buses' bumpContentEpoch() beside it was
    // tried and REMOVED: each was ablated separately and the gate passed
    // without either, so both were code no sabotage could bite. That is 45's
    // rule stated a second time -- invalidateRenderPath(), never
    // bumpRenderChainEpoch() -- and it is why the buses need no epoch
    // handling of their own.
    invalidateRenderPath();
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

    for( SLink *lk : sendLaneRefs_ ) delete lk;
    sendLaneRefs_.clear();
    sendLanes_.clear();

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
    for( SLink *lk : sendLaneRefs_ ) if( lk ) out.append( lk );
    return out;
}

SObject *SStdMixer::systemLaneAt( int sentinel ) const
{
    // -1 is the master. The send sentinels (-2, -3, ...) are RESERVED and
    // answer null until proposal 45 M7 builds them, which is also what stops
    // strackpath::findPathRec's descending scan before it runs off the end.
    if( sentinel == strackpath::SPATH_MASTER )
        return static_cast<SObject *>( masterLane_ );
    // -2 - k is send lane k (proposal 45 M7). Answering null past the last one
    // is also what stops strackpath::findPathRec's descending scan.
    const int k = -2 - sentinel;
    if( k >= 0 && k < sendLanes_.size() )
        return static_cast<SObject *>( sendLanes_[k] );
    return nullptr;
}

int SStdMixer::systemLaneIndexNamed( const QString &name ) const
{
    for( int k = 0; k < sendLanes_.size(); ++k )
        if( sendLanes_[k] && sendLanes_[k]->getSName() == name ) return k;
    return -1;
}

int SStdMixer::systemLaneSentinelOf( const SObject *lane ) const
{
    if( lane && lane == static_cast<const SObject *>( masterLane_ ) )
        return strackpath::SPATH_MASTER;
    for( int k = 0; k < sendLanes_.size(); ++k )
        if( lane && lane == static_cast<const SObject *>( sendLanes_[k] ) )
            return strackpath::spathSendSentinel( k );
    return 0;
}


// --- the master sum's input shape, for AC7.4's gate --------------------------

int SStdMixer::masterInputCount() const
{
    // BUS 0, which is the bus wireMasterChain() feeds into the master lane's
    // chain (D3). A project has had one bus since setNBusses(1), and averaging
    // over several would hide exactly the per-bus mistake this exposes.
    std::shared_ptr<twMixer> mix = masterMixComponent();
    return mix ? (int) mix->getNInputs() : -1;
}

double SStdMixer::masterInputLevelDb( int i ) const
{
    std::shared_ptr<twMixer> mix = masterMixComponent();
    if( !mix || i < 0 || i >= (int) mix->getNInputs() ) return 0.0;
    return mix->inputLevel( (idx_t) i );
}

bool SStdMixer::masterInputWired( int i ) const
{
    std::shared_ptr<twMixer> mix = masterMixComponent();
    if( !mix || i < 0 || i >= (int) mix->getNInputs() ) return false;
    return mix->getInputPlug( (idx_t) i ) != nullptr;
}

// --- send lanes (proposal 45 M7 / D10) --------------------------------------

STrack *SStdMixer::sendLaneNamed( const QString &name ) const
{
    for( STrack *t : sendLanes_ )
        if( t && t->getSName() == name ) return t;
    return nullptr;
}

STrack *SStdMixer::addSendLane( const QString &name )
{
    // AC7.5: A NAME COLLISION IS REFUSED. The name is the address a script and
    // a user reach the lane by (`$send:Reverb`), so two lanes sharing one is
    // not a cosmetic problem -- it makes one of them unaddressable and makes
    // which one you get depend on creation order.
    if( name.trimmed().isEmpty() ) return nullptr;
    if( sendLaneNamed( name ) )     return nullptr;

    SProject *project = dynamic_cast<SProject *>( parent() );
    STrack *lane = new STrack( project );
    lane->setSystemRole( SSystemRole::Send );
    lane->setSName( name );
    adoptSendLane( lane );
    return lane;
}

int SStdMixer::detachSendLane( STrack *lane )
{
    const int k = sendLanes_.indexOf( lane );
    if( k < 0 ) return -1;
    // UNWIRE IT FROM ITS BUS FIRST, WHILE THE BUS IS STILL ALIVE (proposal 47
    // M1). The lane's plugin chain holds an input PLUG into the bus's latch;
    // dropping the bus first leaves that plug dangling and the next setInput()
    // dereferences a destroyed twMixer to detach it. SEGFAULT, found by
    // qxa.send_lane_remove_undo on the UNDO -- remove, restore, crash.
    if( lane ) lane->unwireSendLane();
    sendLanes_.removeAt( k );
    // ...and drop ITS bus, at the SAME index. rewireSendBuses() trims only the
    // TAIL, which is right for a shrink and silently wrong for a removal from
    // the middle -- the two lists would stop being index-parallel and lane j
    // would inherit lane j+1's bus. remove-send-lane is restricted to the last
    // lane today, so that is unreachable; the code does not lean on it.
    if( k < sendBuses_.size() ) sendBuses_.removeAt( k );
    // Deleting the reference link is what releases OUR hold. The lane survives
    // if somebody else pinned it first (an undo step does exactly that) and is
    // deleteLater'd if nobody did.
    delete sendLaneRefs_.takeAt( k );
    // EVERY LANE AFTER IT SHIFTS DOWN ONE SENTINEL, which is why
    // remove-send-lane is restricted to the LAST lane: `-2 - k` IS the address,
    // so removing from the middle silently re-points every path that named a
    // later lane. The restriction lives in the verb, where it can be announced.
    reconnectTracksToMixer();
    return k;
}

void SStdMixer::adoptSendLane( STrack *lane )
{
    if( !lane || sendLanes_.contains( lane ) ) return;
    sendLanes_.append( lane );
    // Our own reference, exactly as the master lane has one and for the same
    // reason: the lane hangs off no child link.
    sendLaneRefs_.append( new SLink( *lane, nullptr ) );
    lane->setRenderPathOwner( this );   // D11

    // ...and it reaches the master SUM through the ordinary rewire pass, which
    // is also what stops the next pass clobbering it (T3). Nothing here wires
    // an input by hand.
    reconnectTracksToMixer();
}

// --- the conductor lane (proposal 45 M6 / D7) -------------------------------

STrack *SStdMixer::conductorLane() const
{
    if( !masterLane_ ) return nullptr;
    // The FIRST conductor child, by role rather than by index. Index 0 is where
    // ensureConductorLane() puts it and where `$master,0` addresses it, but a
    // lookup that TRUSTED index 0 would answer "the conductor" for whatever a
    // future milestone parents to the master first.
    for( SLink *lk : masterLane_->childLinks() ) {
        if( !lk ) continue;
        SObject *so = &lk->getSObject();
        if( so && so->systemRole() == SSystemRole::Conductor )
            return dynamic_cast<STrack *>( so );
    }
    return nullptr;
}

void SStdMixer::ensureConductorLane()
{
    if( !masterLane_ || conductorLane() ) return;

    // A Qt child of the PROJECT, like every other track, which is what gets it
    // serialized: SProject::serialize() writes each of its SObject children as
    // an element, and the <SLink objectId> below -- an ordinary child link of
    // the master lane's own element -- is the reference the loader resolves.
    // Nothing new on either side.
    SProject *project = dynamic_cast<SProject *>( parent() );
    STrack *lane = new STrack( project );
    lane->setSystemRole( SSystemRole::Conductor );
    lane->setSName( QStringLiteral( "Conductor" ) );

    // SLink ctor note (the same one reparent-track spells out): build with no
    // parent, then setParent, so childEvent fires on a fully constructed link.
    SLink *link = new SLink( *lane, nullptr );
    link->setParent( masterLane_ );
    const int landing = masterLane_->childCount() - 1;
    if( landing > 0 ) masterLane_->moveChildToIndex( landing, 0 );

    // IT CONTRIBUTES NO AUDIO FOR TWO INDEPENDENT REASONS, and both are worth
    // knowing because either alone would be enough and neither is obvious:
    //
    //  1. It carries no clips and no instrument -- acceptsClips() is false for
    //     every systemRole (D6), so there is nothing for it to render.
    //  2. THE MASTER LANE'S OWN twTrackMix DRIVES NOTHING. wireAsMasterLane()
    //     takes the mixer's sum straight into the lane's plugin chain and the
    //     chain's output into the mixer's rewire (D3), deliberately bypassing
    //     the lane's trackmix -- which is AC2.8, and which means a CHILD of the
    //     master lane is summed by a component that reaches no output.
    //
    // A change that re-wired the master lane's trackmix would silently make
    // reason 2 false; reason 1 is what would still hold. Stated here so a
    // later milestone that gives a conductor lane real content knows it must
    // not rely on either by accident.
}

void SStdMixer::wireMasterChain()
{
    // PROPOSAL 45 M2 / D3 -- the master lane enters the signal path:
    //
    //   tracks -> twMixer (sum) -> twPluginChain -> twGainStage -> twRewire
    //                              \____________________________/
    //                                owned by the master lane
    //
    // THE ROOT COMPONENT IS STILL THE twRewire, and that is the point. The
    // master meter, RenderSession, AudioEngine and twSpeaker all reach the
    // graph through SProject::getRootComponent()->getRootComponent(); none of
    // them changes, and the master meter becomes post-master-FX by
    // construction rather than by a second tap. Making the master LANE's own
    // rewire the project root was rejected: it moves what every one of those
    // resolves to, for nothing.
    //
    // THE LANE'S OWN twTrackMix AND twRewire GO INERT HERE, deliberately (T5).
    // An STrack builds trackmix -> chain -> gain -> rewire in its constructor;
    // this re-points the CHAIN's input at the mixer's sum, so the lane's
    // trackmix drives nothing, and takes the GAIN's output into the mixer's
    // rewire, so the lane's own rewire is redundant with it. Neither is to be
    // "wired for consistency" later: the master lane has no clips (D6), and a
    // second rewire in the path would be a second page cache to invalidate.
    if( cpMixers_.empty() || !cpMixers_[0] || !cpRewire_ ) return;

    // WHAT GOES BETWEEN THE TWO ENDPOINTS IS THE TRACK'S BUSINESS, and it has
    // to be: app/objects/mixer may include tw/core, tw/graph, tw/mix and
    // tw/schedule and NOT tw/plugins, so this module cannot name
    // twPluginChain at all (tools/check_layering.py enforces it). The mixer
    // hands over its sum and its rewire; STrack wires its own internals.
    if( !masterLane_ || !masterLane_->pluginChainComponent()
                     || !masterLane_->gainStageComponent() ) {
        // No lane yet (the constructor wires the busses BEFORE minting one) or
        // a lane whose components have not been built. Fall back to the
        // pre-M2 topology rather than leaving the rewire unfed -- silence is
        // the one outcome that must not be reachable from a missing lane.
        // ANNOUNCED, never silent: reaching here after the constructor has
        // minted a lane means the lane lost its components, and the symptom
        // downstream would be a master fader and master inserts that do
        // nothing at all.
        TW_LOGW( "model", "[MASTER] no lane components -- the master chain is "
                          "NOT in the signal path" );
        cpRewire_->setInput( 0, cpMixers_[0]->linkOutput( 0 ) );
        return;
    }

    masterLane_->wireAsMasterLane( cpMixers_[0], cpRewire_ );
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
    masterLane_->setRenderPathOwner( this );   // D11

    // The file's lane replaces the constructor's, so the graph must follow it.
    // Without this the sum keeps running through the RETIRED lane's chain --
    // which is about to be deleted, and whose inserts are not the ones the
    // user saved.
    wireMasterChain();

    // A PROJECT WRITTEN BEFORE M6 GAINS ONE HERE. The adopted lane brings its
    // own conductor when the file had one -- it is an ordinary child link, so
    // it arrived with the lane -- and ensureConductorLane() is idempotent, so
    // this mints only in the older case.
    ensureConductorLane();
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
    masterLane_->setRenderPathOwner( this );   // D11

    // ...and its conductor lane (M6/D7), as an ordinary child link of it.
    ensureConductorLane();

    // setNBusses(1) above ran before the lane existed and wired the sum
    // straight into the rewire; now that there is a lane, put its chain in
    // between (D3).
    wireMasterChain();
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
    // The SEND LANES (proposal 45 M7), by the same mechanism and IN ORDER --
    // the order IS the addressing, since sentinel `-2 - k` names the k-th.
    // Written only when there are any, so every project written before M7 and
    // every project that never made one re-serializes byte-identically.
    if( !sendLanes_.isEmpty() ) {
        o << " sendLaneIds='";
        for( int k = 0; k < sendLanes_.size(); ++k ) {
            if( k ) o << ",";
            o << reinterpret_cast<std::uintptr_t>( (SObject *) sendLanes_[k] );
        }
        o << "'";
    }
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
                TW_LOGW( "model", "[MASTER] masterLaneId %s not found in the "
                                  "project; keeping the fresh one",
                         laneId.toStdString().c_str() );
                return;
            }
            STrack *lane = dynamic_cast<STrack *>( &laneLink->getSObject() );
            if( !lane ) {
                TW_LOGW( "model", "[MASTER] masterLaneId %s does not name a "
                                  "track; keeping the fresh one",
                         laneId.toStdString().c_str() );
                return;
            }
            if( lane->systemRole() != SSystemRole::Master ) {
                TW_LOGW( "model", "[MASTER] masterLaneId %s names a track whose "
                                  "systemRole is not master (an older build may "
                                  "have re-saved this project); keeping the "
                                  "fresh one", laneId.toStdString().c_str() );
                return;
            }
            mixer->adoptMasterLane( lane );
        } );
    }

    // The SEND LANES, deferred for exactly the masterLaneId reason: a plain
    // attribute is invisible to the loader's <SLink objectId> dependency
    // ordering, so the lanes' own <STrack> elements may not exist yet.
    //
    // ADOPTED IN FILE ORDER, and a reference that does not name a SEND is
    // SKIPPED rather than adopted -- the AC1.5 rule the master lane already
    // follows. An older build re-saving this file drops systemRole= along with
    // sendLaneIds=, so a file can legitimately come back naming a track that
    // is now an ordinary one; adopting it would put a user track into the
    // master sum TWICE. Skipping shifts the sentinels of the lanes after it,
    // which is the lesser evil and is announced.
    const QString sendIds = element.attribute( "sendLaneIds" );
    if( !sendIds.isEmpty() ) {
        SProjectLoader *loader = &projectLoader;
        const QStringList ids = sendIds.split( ",", Qt::SkipEmptyParts );
        projectLoader.deferResolve( [loader, mixer, ids]() {
            for( const QString &id : ids ) {
                SLink *lk = loader->getObjectDictionary().value( id.trimmed() );
                STrack *lane = lk ? dynamic_cast<STrack *>( &lk->getSObject() )
                                  : nullptr;
                if( !lane || lane->systemRole() != SSystemRole::Send ) {
                    TW_LOGW( "model", "[SEND] sendLaneIds entry %s is missing "
                                      "or does not name a send lane; skipping "
                                      "it", id.toStdString().c_str() );
                    continue;
                }
                mixer->adoptSendLane( lane );
            }
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
