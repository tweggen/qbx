#ifndef SPLACEMENTS_H
#define SPLACEMENTS_H

#include "app/model/sclipwindow.h"
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
 * SObject::isLane() (STrack and SStdMixer return true; proposal 41 D3 split
 * it from isPathContainer(), which merely scopes generic path descent — a
 * fragment answers the latter true and this one false), so this
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
    return ( obj && obj->isLane() ) ? obj : nullptr;
}

// Resolve `path` from `root` to a generic path CONTAINER (proposal 41 M2b):
// anything the index-path search may descend into, which since D3 is a
// STRICTLY WIDER set than isLane() -- a lane fragment answers isPathContainer()
// true and isLane() false. This is what makes a clip's PARENT resolvable when
// that parent is a fragment, while laneAt() -- the placement DESTINATION
// resolver -- stays exactly as strict as it always was: a clip still cannot be
// moved into or out of a fragment (unpack-clips remains the only way out), it
// can only be addressed once it is already there. Null if the path is
// dangling or the object is not a path container at all (e.g. a plain clip).
inline SObject *containerAt( SObject *root, const QList<int> &path )
{
    SObject *obj = strackpath::resolveByPath( root, path );
    return ( obj && obj->isPathContainer() ) ? obj : nullptr;
}

// --- Root-qualified resolution (proposal 09 D21, extended by proposal 41 M2b)
//
// The root a qualified path names: the MASTER when the qualifier is empty,
// else a registered ARRANGEMENT of that name, else -- new in M2b -- a
// registered ASSET of that name whose windowed content is a lane FRAGMENT
// (a non-lane path container), in which case the qualifier addresses the
// fragment's own children ("MyLoop:0" = the first clip packed into "MyLoop",
// the same shape "Drums:0" already has for an arrangement). An UNKNOWN name
// still yields null and NEVER falls back to the master -- that refusal is the
// safety property the whole scheme buys, and a fallback would reintroduce
// exactly the silent wrong-tree edit it exists to prevent.
//
// Arrangements WIN a name collision, so every path that already resolved
// keeps its current meaning; the asset fallback only ever fires where a name
// resolves to nothing today. An asset over a plain lane (STrack/SStdMixer,
// e.g. a folder-track "container asset") gains NO second name here -- that
// lane is already addressable by itself, and isLane() screens it out below.
// Addressing is by the asset's NAME, deliberately never reachable through a
// placement path (proposal 41 M2b D2): editing "through a placement" would
// falsely imply the edit is local to that one placement, when every edit
// inside a fragment is shared by construction (D2 -- one asset, N placements).
inline SObject *rootNamed( SProject *project, const QString &rootName )
{
    if( !project ) return nullptr;
    if( rootName.isEmpty() ) return project->getRootComponent();
    if( SObject *arr = project->arrangement( rootName ) ) return arr;
    if( SObject *asset = project->asset( rootName ) ) {
        if( SClipWindow *w = SClipWindow::of( asset ) ) {
            SObject &content = w->windowContent();
            if( content.isPathContainer() && !content.isLane() ) return &content;
        }
    }
    return nullptr;
}

// Resolve a qualified spec ("Drums:0,1", or "0,1" for the master) to a lane.
// Null for an unknown root, a dangling path, or a non-container target.
inline SObject *laneAtQualified( SProject *project, const QString &spec )
{
    const strackpath::QualifiedPath q = strackpath::parseQualified( spec );
    SObject *root = rootNamed( project, q.root );
    if( !root ) return nullptr;
    return laneAt( root, q.idx );
}

// Resolve a clip PLACEMENT: `path` addresses a child link of a path
// CONTAINER (last index = link index within it). Null for dangling paths or
// when the addressed child is itself a lane (a nested track, not a clip).
//
// Proposal 41 M2b: the parent resolves through containerAt(), not laneAt() --
// a fragment is a path container but never a lane, so this is what makes a
// clip's PROPERTY editable once it is already inside one (resize-clip,
// set-pitch, ...), while every verb whose destination resolves through
// laneAt() directly (place-clip, pack-clips' own lane check, ...) keeps
// refusing to move a clip into or out of a fragment. The final check is
// UNCHANGED and still uses isLane(): a nested track is still never returned
// as if it were a clip, fragment or not.
inline SLink *placementAt( SObject *root, const QList<int> &path )
{
    if( path.isEmpty() ) return nullptr;
    QList<int> containerPath = path;
    int idx = containerPath.takeLast();
    SObject *container = containerAt( root, containerPath );
    SLink *lk = container ? container->childAt( idx ) : nullptr;
    if( !lk || lk->getSObject().isLane() ) return nullptr;
    return lk;
}

}  // namespace splacements

#endif
