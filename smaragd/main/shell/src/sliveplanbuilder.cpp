#include "app/shell/sliveplanbuilder.h"

#include <algorithm>
#include <map>

#include "app/model/slink.h"
#include "app/model/sobject.h"
#include "app/model/ssolorules.h"
#include "app/objects/mixer/sstdmixer.h"
#include "app/objects/track/spluginchain.h"
#include "app/objects/track/spluginslot.h"
#include "app/objects/track/strack.h"
#include "tw/core/twlog.h"
#include "tw/mix/twgainstage.h"
#include "tw/mix/twmixer.h"
#include "tw/mix/twrewire.h"

bool SLiveClosure::contains( const STrack *t ) const
{
    return std::find( ordered.begin(), ordered.end(), t ) != ordered.end();
}

namespace sliveplan {

bool isAudioInput( const STrack *t )
{
    return t && t->getTrackInput().startsWith( QStringLiteral( "audio:" ) );
}

bool isMidiInput( const STrack *t )
{
    return t && t->hasMidiTrackInput();
}

bool metronomeWanted( bool metronomeOn, bool playing, bool recording,
                      bool countIn, bool clickWhileRecording )
{
    // THE COUNT-IN IS NOT CONDITIONAL ON THE METRONOME SWITCH. A count-in with
    // no click is a silent wait, which is not a feature anybody asked for; the
    // click is what a count-in IS. Cubase / Logic / REAPER all behave this way.
    if( countIn ) return true;
    if( !metronomeOn ) return false;
    // WHILE RECORDING, `clickWhileRecording` is the sole authority - not
    // `playing || recording`, because recording implies the transport is
    // running and an `||` would make the "Click while recording" checkbox
    // able to silence the take but never able to silence a plain click that
    // just happens to sound "while recording" too. Ordinary playback (not
    // recording) is unaffected either way.
    if( recording ) return clickWhileRecording;
    return playing;
}


namespace {

// One walk of the lane tree that records depth and parent for every track.
// Nothing here touches the engine: it is model reads on the main thread, the
// same discipline ssolorules.h states.
struct TreeInfo {
    std::map<STrack *, STrack *> parent;   // null parent == direct child of the mixer
    std::map<STrack *, int>      depth;
    std::vector<STrack *>        all;
};

void walk( SObject *node, STrack *parentTrack, int depth, TreeInfo &info )
{
    for( SLink *lk : node->childLinks() ) {
        if( !lk ) continue;
        SObject *child = &lk->getSObject();
        if( !child->isPathContainer() ) continue;
        STrack *t = dynamic_cast<STrack *>( child );
        if( t ) {
            info.parent[t] = parentTrack;
            info.depth[t]  = depth;
            info.all.push_back( t );
        }
        walk( child, t ? t : parentTrack, depth + 1, info );
    }
}

// WHICH SLOT 0 WILL SOUND THIS TRACK'S LIVE NOTES (design D4).
//
// The same rule STrack::eventFeed() applies, read upward: a track that holds an
// instrument CONSUMES the events; otherwise they go to the parent iff this
// track bubbles them up (`midiRouting`), and the question repeats there. A
// track that neither sounds nor bubbles is the end of the line and the answer
// is "nothing" - which is a legitimate answer and is why the caller must not
// treat every armed MIDI track as a live source.
STrack *midiConsumerIn( const TreeInfo &info, STrack *t )
{
    for( STrack *cur = t; cur; ) {
        if( cur->instrumentSlot() ) return cur;
        if( !cur->bubblesEventsUp() ) return nullptr;
        auto it = info.parent.find( cur );
        cur = ( it == info.parent.end() ) ? nullptr : it->second;
    }
    return nullptr;
}

}  // namespace

STrack *midiConsumerFor( SObject *rootMixer, STrack *t )
{
    if( !rootMixer || !t ) return nullptr;
    TreeInfo info;
    walk( rootMixer, nullptr, 0, info );
    return midiConsumerIn( info, t );
}

SLiveClosure computeClosure( SObject *rootMixer, bool playing, bool recording,
                             const std::vector<const STrack *> &inertlyArmed )
{
    SLiveClosure out;
    if( !rootMixer ) return out;

    TreeInfo info;
    walk( rootMixer, nullptr, 0, info );

    // 1. The SOURCES: (armed AND monitorEffective) OR monitorMode == on,
    //    intersected with "has an input this phase can render".
    //
    // AN AUDIO INPUT MAKES ITS OWN TRACK A SOURCE. A MIDI INPUT MAKES ITS
    // CONSUMER ONE (design D4, section 3 case (iii)): the notes are heard on
    // whatever slot 0 will sound them, which is the track itself when it holds
    // an instrument and the folder above it when the child bubbles its events
    // up. The armed CHILD then stays in the frozen sum - it is a MIDI source,
    // not an audio one, and its own clips must keep playing.
    for( STrack *t : info.all ) {
        const bool audio = sliveplan::isAudioInput( t );
        const bool midi  = sliveplan::isMidiInput( t );
        if( !audio && !midi ) continue;
        const bool inert =
            std::find( inertlyArmed.begin(), inertlyArmed.end(),
                       (const STrack *) t ) != inertlyArmed.end();
        const bool armed = t->isArmedForRecording() && !inert;
        const bool want  = ( armed && t->monitorEffective( playing, recording ) )
                           || t->getMonitorMode() == STrack::MonitorMode::On;
        if( !want ) continue;
        if( audio ) {
            out.sources.push_back( t );
            continue;
        }
        STrack *consumer = midiConsumerIn( info, t );
        if( !consumer ) {
            // Nothing would sound these notes. Excluding the track from the
            // frozen sum would silence its clips for no gain (design D3), so
            // it is simply not a live source.
            continue;
        }
        SLiveMidiFeed feed;
        feed.armed    = t;
        feed.consumer = consumer;
        feed.port     = t->trackInputMidiPort();
        feed.channel  = t->trackInputMidiChannel();
        out.midiFeeds.push_back( feed );
        if( std::find( out.sources.begin(), out.sources.end(), consumer )
            == out.sources.end() )
            out.sources.push_back( consumer );
    }
    if( out.sources.empty() ) return out;

    // 2. THE CLOSURE: each source plus every folder above it, up to (but not
    //    including) the root mixer, which is not a track.
    std::vector<STrack *> members;
    for( STrack *s : out.sources ) {
        for( STrack *cur = s; cur; cur = info.parent[cur] ) {
            if( std::find( members.begin(), members.end(), cur ) == members.end() )
                members.push_back( cur );
        }
    }

    // 3. THE ORDER. Deepest first, so a parent's liveChildren indices are all
    //    strictly less than its own (twLivePlan::finalize() checks it).
    std::stable_sort( members.begin(), members.end(),
                      [&info]( STrack *a, STrack *b ) {
                          return info.depth[a] > info.depth[b];
                      } );
    out.ordered = members;

    for( STrack *t : out.ordered )
        if( info.parent[t] == nullptr ) out.topLevel.push_back( t );

    return out;
}

}  // namespace sliveplan

namespace {

// The plugin slots of a track, in slot order, as processors. A null processor
// is left in place: twLiveTrackPlan::inserts skips nulls, which is how a chain
// keeps its shape while a plugin is missing.
void collectInserts( STrack *t, twLiveTrackPlan &out )
{
    SPluginChain *chain = t->getPluginChain();
    if( !chain ) return;
    const int n = chain->getSlotCount();
    for( int i = 0; i < n; ++i ) {
        SPluginSlot *slot = chain->getSlotAt( i );
        if( !slot ) continue;
        out.inserts.push_back( slot->getProcessor() );
    }
}

}  // namespace

std::shared_ptr<twLivePlan>
SLivePlanBuilder::build( const SLiveClosure &closure, const Params &params,
                         const SourceFn &sourceFor )
{
    if( closure.empty() || !params.mixer ) return nullptr;
    // A metronome-only lane is legal (proposal 21 L5): press Play with the
    // click on and nothing armed, and the plan is one synthetic track.
    if( closure.ordered.empty() && !params.metronome ) return nullptr;

    auto plan = std::make_shared<twLivePlan>();
    plan->blockFrames    = params.blockFrames;
    plan->sampleRate     = params.sampleRate;
    plan->leadFrames     = params.leadFrames;
    plan->flipEpoch      = params.flipEpoch;
    plan->flipEpochPrime = params.flipEpochPrime;

    // THE TRANSPORT (design D2). Playing: the sequenced feed is ON and the
    // automation follows the block. Stopped: the feed is MASKED and every lane
    // is held at the locator -- no reference DAW plays sequenced notes while
    // the transport is stopped, and an automated parameter must read the value
    // under the stopped playhead rather than sweep along a virtual counter.
    plan->transport.playing          = params.playing;
    plan->transport.feedEnabled      = params.playing;
    plan->transport.holdAutomationAt = params.playing ? (offset_t) -1 : params.locator;
    // The stopped lane's virtual counter, UNCHANGED by a count-in (proposal 21
    // L5): the count-in runs the ordinary stopped lane FORWARD from the
    // locator, and it is the CLICK GRID that is anchored at the record
    // position. Running the counter backwards from `locator - N bars` was the
    // first design and is wrong for a reason worth recording: at a locator
    // inside the first N bars it produces NEGATIVE positions, and a ring entry
    // stamped below zero is discarded by twlive::gateEpoch as an unwritten
    // slot -- so a count-in at bar 1, the commonest case there is, would have
    // been silent.
    plan->stoppedAnchor              = params.locator;

    // THE MASTER-SHAPE PRECONDITION (design D3), checked on every build rather
    // than assumed. LinearSplit means the RT adds the ring to the frozen root
    // page; Closure means the master joins the pump's closure and the RT pops
    // the ring only.
    const std::shared_ptr<twMixer>  masterMix  = params.mixer->masterMixComponent();
    const std::shared_ptr<twRewire> masterRoot = params.mixer->masterRewireComponent();
    const twlive::twMasterShape shape =
        twlive::checkMasterShape( masterMix.get(), masterRoot.get(), params.width );
    plan->masterLinear = shape.linear();

    std::map<const STrack *, int> indexOf;
    SObject   *root    = params.mixer;
    const bool anySolo = ssolo::anySoloInTree( root );

    for( STrack *t : closure.ordered ) {
        twLiveTrackPlan tp;
        tp.name     = t->getSName().toStdString();
        tp.channels = (idx_t) t->getChannels();
        if( tp.channels < 1 ) tp.channels = 1;
        collectInserts( t, tp );
        if( t->gainStageComponent() )
            tp.gain = t->gainStageComponent()->envelope();
        if( auto r = std::dynamic_pointer_cast<twRewire>( t->getRootComponent() ) )
            tp.channelMap = r->channelMap();

        // ONLY an AUDIO source gets an input. An instrument track driven live
        // from MIDI is design section 3 case (ii): slot 0 is a generator and
        // the slot's audio input is silence, so asking for one here would open
        // the machine's microphone for a track that never wanted it.
        if( sliveplan::isAudioInput( t )
            && std::find( closure.sources.begin(), closure.sources.end(), t )
                   != closure.sources.end() )
            tp.input = sourceFor ? sourceFor( t ) : nullptr;

        // The children. A child track in the closure is a LIVE child (already
        // in the vector, because the order is deepest-first); an audible child
        // that is NOT is a FROZEN input, read by position out of its own root
        // pages. The readahead re-roots a demand at each of those (SLiveMonitor).
        for( SLink *lk : t->childLinks() ) {
            if( !lk ) continue;
            STrack *child = dynamic_cast<STrack *>( &lk->getSObject() );
            if( !child ) continue;
            auto it = indexOf.find( child );
            if( it != indexOf.end() ) {
                tp.liveChildren.push_back( it->second );
            } else if( ssolo::isLaneAudible( root, child, anySolo ) ) {
                tp.frozenInputs.push_back( child->getRootComponent() );
            }
        }

        indexOf[t] = (int) plan->tracks.size();
        plan->tracks.push_back( std::move( tp ) );
    }

    // THE TOP OF THE CLOSURE. One top-level member under a linear master is
    // the whole plan: the RT adds its ring straight onto the root page. More
    // than one -- or a master that is NOT a unity sum with an identity map --
    // needs a node that sums them, and that node is shaped exactly like a
    // folder: no input, no inserts, unity gain, its members as liveChildren.
    // Under Closure it additionally reads every UNARMED top-level track's
    // frozen root, which IS "the pump renders the master" (design D3).
    // THE CLICK (proposal 21 L5, design D1). One synthetic track, appended
    // AFTER every closure member so the topological order finalize() checks
    // still holds, with no input device, no inserts, unity gain and an identity
    // map. It owns no STrack, is in no `indexOf`, and nothing above ever asks
    // whether it is armed - which is exactly why it perturbs nothing.
    int metroIndex = -1;
    if( params.metronome ) {
        twLiveTrackPlan m;
        m.name     = "metronome";
        m.channels = params.width < 1 ? 1 : params.width;
        m.input    = params.metronome;
        metroIndex = (int) plan->tracks.size();
        plan->tracks.push_back( std::move( m ) );
    }

    if( closure.topLevel.empty() ) {
        // METRONOME ONLY: the click IS the output. A topLevel that is empty
        // while the closure is NOT cannot happen - the walk stops at the root
        // mixer, so every member has an ancestor that is a direct child of it -
        // but front() on an empty vector is not a diagnosis, it is a crash, so
        // the case is handled rather than assumed away.
        if( metroIndex < 0 ) return nullptr;
        plan->outputTrack = metroIndex;
        if( !plan->finalize() ) return nullptr;
        return plan;
    }
    const bool needSum = closure.topLevel.size() > 1 || !plan->masterLinear
                         || metroIndex >= 0;
    if( !needSum ) {
        plan->outputTrack = indexOf[closure.topLevel.front()];
    } else {
        twLiveTrackPlan master;
        master.name     = plan->masterLinear ? "live sum" : "master";
        master.channels = params.width < 1 ? 1 : params.width;
        for( STrack *t : closure.topLevel ) master.liveChildren.push_back( indexOf[t] );
        if( metroIndex >= 0 ) master.liveChildren.push_back( metroIndex );
        if( !plan->masterLinear ) {
            for( SLink *lk : params.mixer->childLinks() ) {
                if( !lk ) continue;
                STrack *t = dynamic_cast<STrack *>( &lk->getSObject() );
                if( !t || closure.contains( t ) ) continue;
                if( ssolo::isLaneAudible( root, t, anySolo ) )
                    master.frozenInputs.push_back( t->getRootComponent() );
            }
            if( masterRoot ) master.channelMap = masterRoot->channelMap();
            TW_LOGW( "shell",
                     "[LIVE] master is not a linear identity (%s): the pump "
                     "renders it and the RT pops the ring only",
                     shape.reason );
        }
        plan->outputTrack = (int) plan->tracks.size();
        plan->tracks.push_back( std::move( master ) );
    }

    if( !plan->finalize() ) return nullptr;
    return plan;
}
