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

}  // namespace

SLiveClosure computeClosure( SObject *rootMixer, bool playing, bool recording,
                             const std::vector<const STrack *> &inertlyArmed )
{
    SLiveClosure out;
    if( !rootMixer ) return out;

    TreeInfo info;
    walk( rootMixer, nullptr, 0, info );

    // 1. The SOURCES: (armed AND monitorEffective) OR monitorMode == on,
    //    intersected with "has an input this phase can render".
    for( STrack *t : info.all ) {
        if( !sliveplan::isAudioInput( t ) ) continue;
        const bool inert =
            std::find( inertlyArmed.begin(), inertlyArmed.end(),
                       (const STrack *) t ) != inertlyArmed.end();
        const bool armed = t->isArmedForRecording() && !inert;
        const bool want  = ( armed && t->monitorEffective( playing, recording ) )
                           || t->getMonitorMode() == STrack::MonitorMode::On;
        if( want ) out.sources.push_back( t );
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

        if( std::find( closure.sources.begin(), closure.sources.end(), t )
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
    // topLevel cannot be empty for a non-empty closure - the walk stops at the
    // root mixer, so every member has an ancestor that is a direct child of it
    // - but front() on an empty vector is not a diagnosis, it is a crash.
    if( closure.topLevel.empty() ) return nullptr;
    const bool needSum = closure.topLevel.size() > 1 || !plan->masterLinear;
    if( !needSum ) {
        plan->outputTrack = indexOf[closure.topLevel.front()];
    } else {
        twLiveTrackPlan master;
        master.name     = plan->masterLinear ? "live sum" : "master";
        master.channels = params.width < 1 ? 1 : params.width;
        for( STrack *t : closure.topLevel ) master.liveChildren.push_back( indexOf[t] );
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
