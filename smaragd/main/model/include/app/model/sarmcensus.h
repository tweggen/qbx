#ifndef SARMCENSUS_H
#define SARMCENSUS_H

#include "app/model/slink.h"
#include "app/model/sobject.h"
#include "app/model/sproject.h"

#include <QList>

/**
 * WHICH LANES ARE ARMED FOR RECORDING, over the WHOLE PROJECT.
 *
 * The arm flag is a plain per-lane boolean (`SObject::isArmedForRecording()`),
 * so there is no audibility rule here and nothing to get subtly wrong — but
 * "which lanes, project-wide" is still a question nobody could ask: every
 * existing reader walks the root it already has (the recorders walk the mixer,
 * the live plan walks the lane tree it is building a plan for). The status
 * bar's badge has to answer for lanes in roots nobody is looking at, which is
 * the whole reason it exists.
 *
 * A SIBLING OF `ssolo::soloedLanesInProject()`, NOT A SHARED WALK, and
 * deliberately: that function lives with the audibility RULE because solo's
 * effect IS that rule (in Global scope it follows the reference graph, not the
 * lane tree, and ssolorules.h's header spends forty lines on why). Arm has no
 * such rule — it never travels through a placement, because what is armed is
 * the lane the user armed — so folding the two walks together would put a
 * plain flag behind a doorway marked "here be solo scope".
 *
 * THE LANE TREE ONLY, every root. Only lanes carry the flag
 * (`SObject::isLane()`, proposal 41 D3), so the walk never descends into a
 * clip, and never into a fragment either: a fragment is a path container but
 * not a lane.
 *
 * NOTE ON AN ARM THAT IS NOT LIVE. Since QBX-260 a project that was SAVED with
 * a track armed comes back armed but INERT — `SLiveMonitor` keeps it out of the
 * live set until the user arms it in this session, so loading a project does
 * not open the microphone (design D9). Such a lane is still armed, and this
 * census still reports it: that is the same answer the track's own Arm button
 * gives, and a badge that disagreed with the button would be the worse lie.
 */
namespace sarm {

namespace detail {

inline void collectArmedLanes( SObject *obj, QList<SObject *> &out )
{
    if( !obj ) return;
    for( SLink *lk : obj->childLinks() ) {
        if( !lk ) continue;
        SObject *child = &lk->getSObject();
        if( !child->isLane() ) continue;
        if( child->isArmedForRecording() ) out.append( child );
        collectArmedLanes( child, out );
    }
}

}  // namespace detail

/** Every armed lane in the project, master root first, then the arrangements
 *  in root order; parents before their children. */
inline QList<SObject *> armedLanesInProject( SProject *project )
{
    QList<SObject *> out;
    if( !project ) return out;
    // check_root_resolution: master -- enumerates EVERY root -- the master here, then project->arrangements() below
    if( SObject *master = project->masterRoot() )
        detail::collectArmedLanes( master, out );
    for( SObject *arr : project->arrangements() )
        detail::collectArmedLanes( arr, out );
    return out;
}

/** Is anything in the project armed? The predicate behind the badge's
 *  existence and the Disarm-all command's enabled state. */
inline bool anyArmedInProject( SProject *project )
{
    return !armedLanesInProject( project ).isEmpty();
}

}  // namespace sarm

#endif  // SARMCENSUS_H
