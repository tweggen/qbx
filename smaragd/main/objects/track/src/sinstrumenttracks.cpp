#include "app/objects/track/sinstrumenttracks.h"

#include "app/model/sobject.h"
#include "app/model/slink.h"
#include "app/objects/track/strack.h"

namespace sinstruments {

namespace {

void collectRec( SObject *node, std::vector<STrack *> &out )
{
    if( !node ) return;
    for( SLink *lk : node->childLinks() ) {
        if( !lk ) continue;
        SObject *child = &lk->getSObject();
        // Lanes only. A clip carries no plugin chain and a take stack is not a
        // lane, so descending into them would only cost time.
        if( !child->isLane() ) continue;
        if( STrack *tr = dynamic_cast<STrack *>( child ) ) {
            if( tr->instrumentSlot() ) out.push_back( tr );
        }
        // A folder track is still walked into even when it holds an instrument
        // of its own: a child lane may hold one too, and both have to be
        // barriered (they are independent processors).
        collectRec( child, out );
    }
}

}  // namespace

void collectInstrumentTracks( SObject *root, std::vector<STrack *> &out )
{
    collectRec( root, out );
}

}  // namespace sinstruments
