#ifndef SPLACEMENTS_H
#define SPLACEMENTS_H

#include "app/model/sobject.h"
#include "app/model/sobjectpath.h"
#include "app/model/sproject.h"

/**
 * The placement service (proposal 14, follow-up to Phase 6): generic
 * container/placement resolution for action code.
 *
 * Most actions that used to dynamic_cast to STrack/SStdMixer only needed
 * (a) "give me a valid root container" and (b) "resolve this path to a lane
 * I may place clips on" — every operation that followed (childAt,
 * indexOfChild, childCount, setParent, setVolume, …) is plain SObject/SLink
 * API. Those casts were the objects-slice cycle. Lane-ness is expressed by
 * SObject::isPathContainer() (STrack and SStdMixer return true), so this
 * header names no concrete types and lives in the model.
 *
 * Actions that genuinely mutate the mixer's lane LIST (insertTrack,
 * removeTrack, reorderTrack, …) are type-specific by nature and live in the
 * mixer slice instead of using this service.
 */
namespace splacements {

// The project's root container, or null when there is no usable root.
inline SObject *rootContainer( SProject *project )
{
    return project ? project->getRootComponent() : nullptr;
}

// Resolve `path` from `root` to a lane (a container clips/placements may be
// parented to: a track, or the root mixer itself). Null if the path is
// dangling or the object is not a lane.
inline SObject *laneAt( SObject *root, const QList<int> &path )
{
    SObject *obj = strackpath::resolveByPath( root, path );
    return ( obj && obj->isPathContainer() ) ? obj : nullptr;
}

// Resolve a clip PLACEMENT: `path` addresses a child link of a lane
// (last index = link index within the lane). Null for dangling paths or
// when the addressed child is itself a lane (a nested track, not a clip).
inline SLink *placementAt( SObject *root, const QList<int> &path )
{
    if( path.isEmpty() ) return nullptr;
    QList<int> lanePath = path;
    int idx = lanePath.takeLast();
    SObject *lane = laneAt( root, lanePath );
    SLink *lk = lane ? lane->childAt( idx ) : nullptr;
    if( !lk || lk->getSObject().isPathContainer() ) return nullptr;
    return lk;
}

}  // namespace splacements

#endif
