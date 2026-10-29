#ifndef SSOLORULES_H
#define SSOLORULES_H

#include "app/model/sobject.h"
#include "app/model/slink.h"
#include "app/model/sproject.h"

#include <QList>
#include <QSet>

/**
 * The ONE audibility rule (mute + solo), shared by every consumer.
 *
 * Solo is not a property of a single lane: whether a lane is heard depends on
 * whether ANYTHING in the whole project is soloed, and on where the soloed
 * lane sits relative to this one. Before this header the rule was written out
 * three times — in SStdMixer::reconnectTracksToMixer(), in the track-head meter
 * and in the Track Detail meter — and all three only looked at the mixer's
 * DIRECT children, so a solo on a lane nested inside a folder track was a
 * complete no-op (and the meters would have disagreed with the ear the moment
 * it wasn't).
 *
 *   audible = !muted && ( !anySoloInForce
 *                         || isSolo
 *                         || hasSoloedDescendant     // a folder holding a soloed lane
 *                         || isDescendantOfSoloed )  // a lane inside a soloed folder
 *
 * Applied at EVERY summing container: the root SStdMixer nulls the input plug
 * of an inaudible direct child, a folder STrack mutes the corresponding clip
 * entry (twTrackMix::setClipMuted). Mute/solo are properties of the summing
 * CHANNEL and are never baked into a track's own rendered output — see the
 * comment above twTrackMix::setClipMuted.
 *
 * Everything here is a plain read of model flags on the UI thread; nothing
 * touches the engine. Only lanes (SObject::isLane(), proposal 41 D3) carry
 * solo, so the walks never descend into clips — and, since M1, never into a
 * fragment either: a fragment answers isPathContainer() true (it is a
 * container) but isLane() false (it has no track identity), so a
 * fragment-internal flag can never darken lanes across the project.
 *
 * --- THE SCOPE IS A USER OPTION (QBX-256) -----------------------------------
 *
 * The four lines above are the rule WITHIN one root. What they are applied
 * ACROSS is `SProject::soloScope()`, which the shell pushes in from the
 * per-user option `mixer/soloScope` (app/model may not read SSettings):
 *
 *   Global (THE DEFAULT) — a solo anywhere in the project is in force
 *     everywhere. Soloing the bass-drum lane inside a drum-loop arrangement
 *     leaves the bass drum as the only thing the project plays. This is what
 *     a solo button means to a user who hits it to fine-tune one sound.
 *   Arrangement — the rule stops at the root that holds the soloed lane
 *     (QBX-231 / QBX-233). A solo inside a sub-arrangement changes what that
 *     arrangement's placements play and nothing else.
 *
 * In Global scope the three solo terms follow the REFERENCE GRAPH rather than
 * the lane tree, because that is how one root reaches another: a lane's clip
 * is an SCut, and the SCut's content link is among its childLinks() (the link
 * is constructed unparented and then setParent(this)), so a placement of an
 * arrangement root is an ordinary edge of the same walk. It is the graph
 * splayhead::detail::findRec walks for the derived playhead, and it carries
 * the same two hazards, handled the same way: a PER-PATH guard (the same body
 * legitimately reached twice at two positions must be visited twice, so a
 * global `seen` set would answer wrongly) and a DEPTH CAP (a cyclic file is a
 * stack overflow on the UI thread, not a wrong pixel;
 * sarrangements::placementWouldCycle refuses the cycle at the edit, and this
 * is the backstop for a file that arrived cyclic anyway).
 *
 * BOTH DIRECTIONS CROSS THE EDGE, and the second one is the trap. "Holds a
 * soloed lane" keeps the master lane that CARRIES the drum loop wired, or the
 * path to the very thing you soloed would be cut and you would hear silence.
 * "Inside a soloed ancestor" is what keeps a sub-arrangement's lanes audible
 * when the MASTER lane carrying it is the soloed one: without it, a
 * project-wide anySolo is true inside every arrangement, where nothing is
 * soloed, so every lane of the loop goes inaudible and its asset capture
 * renders SILENCE — you would solo the lane the loop sits on and hear nothing.
 *
 * The second term is answered FROM THE SOLOED LANE'S SIDE ("does a soloed
 * lane reach this one?") rather than by searching for this lane from every
 * root. The two are equivalent, and the soloed set is normally one lane while
 * the project graph is the whole document — this is the difference between a
 * walk per lane per meter tick and a walk that costs nothing worth measuring.
 * Nothing walks UP: the model's links point down only (see srecordmode.cpp).
 */
namespace ssolo {

// Solo set on `obj` itself or on any lane below it. The LANE TREE only — see
// reachesSoloedLane() for the Global-scope walk that crosses placements.
inline bool soloInSubtree( SObject *obj )
{
    if( !obj ) return false;
    if( obj->isSolo() ) return true;
    for( SLink *lk : obj->childLinks() ) {
        if( !lk ) continue;
        SObject *child = &lk->getSObject();
        if( !child->isLane() ) continue;
        if( soloInSubtree( child ) ) return true;
    }
    return false;
}

// Is solo in force anywhere in THIS TREE? A WHOLE-TREE walk: the historical
// "direct children of the mixer only" version is precisely why a nested solo
// changed nothing. `root` itself is not a lane, so it is not consulted.
//
// This is the per-root primitive. What a CALLER wants is almost always
// anySoloInForce(), which applies the user's scope; the exceptions are
// fragment_test (which asserts about one tree on purpose) and
// anySoloInProject() below, which folds this over every root.
inline bool anySoloInTree( SObject *root )
{
    if( !root ) return false;
    for( SLink *lk : root->childLinks() ) {
        if( !lk ) continue;
        SObject *child = &lk->getSObject();
        if( !child->isLane() ) continue;
        if( soloInSubtree( child ) ) return true;
    }
    return false;
}

namespace detail {

// Depth-first search for `lane`, carrying "some strict ancestor is soloed".
inline bool underSoloedAncestorRec( SObject *cur, SObject *lane, bool ancestorSoloed )
{
    for( SLink *lk : cur->childLinks() ) {
        if( !lk ) continue;
        SObject *child = &lk->getSObject();
        if( !child->isLane() ) continue;
        if( child == lane ) {
            // The same object can be placed under more than one parent; keep
            // looking if THIS placement is not under a soloed ancestor.
            if( ancestorSoloed ) return true;
            continue;
        }
        if( underSoloedAncestorRec( child, lane,
                                    ancestorSoloed || child->isSolo() ) )
            return true;
    }
    return false;
}

}  // namespace detail

// True if `lane` sits (strictly) below a soloed folder — the rule that keeps a
// soloed folder's whole subtree audible.
inline bool underSoloedAncestor( SObject *root, SObject *lane )
{
    if( !root || !lane ) return false;
    return detail::underSoloedAncestorRec( root, lane, false );
}

// --- the scope, and the reference-graph walks it needs (QBX-256) ------------

/** The scope in force for `obj`'s project. A detached object has no project
 *  and so no root registry: it degrades to Arrangement, which is the only
 *  thing a tree with no siblings can honestly answer. */
inline SProject::SoloScope scopeOf( SObject *obj )
{
    SProject *p = obj ? obj->getProjectSafe() : nullptr;
    return p ? p->soloScope() : SProject::SoloScope::Arrangement;
}

namespace detail {

// A cycle in the reference graph is a stack overflow, not a wrong answer, so
// depth is bounded as well as the path set — splayhead::kMaxDepth's reason and
// its number, for the same graph.
static const int kMaxReachDepth = 32;

/** Does `obj` — or anything reachable from it THROUGH PLACEMENTS — carry
 *  solo? `obj` itself counts, which is what makes this the Global-scope
 *  spelling of both "isSolo" and "holds a soloed lane". */
inline bool reachesSoloedLane( SObject *obj, QSet<const SObject *> &onPath,
                               int depth )
{
    if( !obj || depth > kMaxReachDepth ) return false;
    if( onPath.contains( obj ) ) return false;          // per-path guard
    if( obj->isLane() && obj->isSolo() ) return true;
    onPath.insert( obj );
    bool found = false;
    for( SLink *lk : obj->childLinks() ) {
        if( !lk ) continue;
        if( reachesSoloedLane( &lk->getSObject(), onPath, depth + 1 ) ) {
            found = true;
            break;
        }
    }
    onPath.remove( obj );
    return found;
}

/** Is `target` reachable STRICTLY below `from`, through placements? The
 *  Global-scope spelling of "inside a soloed ancestor", asked of each soloed
 *  lane in turn. */
inline bool reachesObject( SObject *from, const SObject *target,
                           QSet<const SObject *> &onPath, int depth )
{
    if( !from || !target || depth > kMaxReachDepth ) return false;
    if( onPath.contains( from ) ) return false;          // per-path guard
    onPath.insert( from );
    bool found = false;
    for( SLink *lk : from->childLinks() ) {
        if( !lk ) continue;
        SObject *child = &lk->getSObject();
        if( child == target ) { found = true; break; }
        if( reachesObject( child, target, onPath, depth + 1 ) ) {
            found = true;
            break;
        }
    }
    onPath.remove( from );
    return found;
}

/** Every soloed lane in `root`'s tree. Lanes only, as solo is a lane
 *  property; appends, so a caller folds it over the roots. */
inline void collectSoloedLanes( SObject *obj, QList<SObject *> &out )
{
    if( !obj ) return;
    for( SLink *lk : obj->childLinks() ) {
        if( !lk ) continue;
        SObject *child = &lk->getSObject();
        if( !child->isLane() ) continue;
        if( child->isSolo() ) out.append( child );
        collectSoloedLanes( child, out );
    }
}

}  // namespace detail

/** Is solo in force anywhere in the PROJECT — every root, not one tree? */
inline bool anySoloInProject( SProject *project )
{
    if( !project ) return false;
    // check_root_resolution: master -- enumerates EVERY root -- the master here, then project->arrangements() below
    if( SObject *master = project->masterRoot() ) {
        if( anySoloInTree( master ) ) return true;
    }
    for( SObject *arr : project->arrangements() ) {
        if( arr && anySoloInTree( arr ) ) return true;
    }
    return false;
}

/**
 * IS SOLO IN FORCE for the lanes under `root`, under the user's scope. THIS
 * is what a caller pre-computing the flag for a pass must use: a caller that
 * folds anySoloInTree() itself would, in Global scope, pass false while
 * another root holds the solo, and the rule below would then never bite.
 */
inline bool anySoloInForce( SObject *root )
{
    if( !root ) return false;
    if( scopeOf( root ) == SProject::SoloScope::Global )
        return anySoloInProject( root->getProjectSafe() );
    return anySoloInTree( root );
}

/** Every soloed lane in the project, in root order. For the Global-scope rule
 *  and for anything that has to SAY where the solo is (QBX-257). */
inline QList<SObject *> soloedLanesInProject( SProject *project )
{
    QList<SObject *> out;
    if( !project ) return out;
    // check_root_resolution: master -- enumerates EVERY root -- the master here, then project->arrangements() below
    if( SObject *master = project->masterRoot() )
        detail::collectSoloedLanes( master, out );
    for( SObject *arr : project->arrangements() )
        detail::collectSoloedLanes( arr, out );
    return out;
}

/**
 * In Global scope: is `lane`'s OWN MATERIAL what was soloed — because the lane
 * carries solo itself, or sits inside a lane that does?
 *
 * The question exists because "audible" is not one thing in Global scope. A
 * master lane that CARRIES the soloed lane's arrangement has to stay audible
 * or the path to the soloed lane is cut — but the clips it holds of its own
 * are not what the user soloed, and letting them through is a solo that
 * leaks (measured: a carrier lane holding one copy of its own played
 * 0.1333 where the soloed lane alone is 0.06665). So the lane stays wired and
 * STrack::applyChildTrackAudibility mutes the clip entries that do not
 * themselves reach a soloed lane. This is the predicate that tells the two
 * cases apart.
 */
inline bool ownMaterialSoloed( SObject *lane )
{
    if( !lane ) return false;
    if( lane->isSolo() ) return true;
    const QList<SObject *> soloed = soloedLanesInProject( lane->getProjectSafe() );
    for( SObject *s : soloed ) {
        if( s == lane ) continue;
        QSet<const SObject *> onPath;
        if( detail::reachesObject( s, lane, onPath, 0 ) ) return true;
    }
    return false;
}

/** Does this clip (or anything it windows) reach a soloed lane? The per-clip
 *  half of ownMaterialSoloed()'s rule. */
inline bool clipReachesSoloedLane( SObject *clip )
{
    if( !clip ) return false;
    QSet<const SObject *> onPath;
    return detail::reachesSoloedLane( clip, onPath, 0 );
}

/**
 * The rule. `root` is the container that HOLDS `lane` (QBX-233's census
 * answer: eleven of twelve callers already did that, and the twelfth was the
 * defect) — in Global scope the solo terms span every root, so `root` then
 * decides nothing and the lane's own project is what is walked. A null root
 * degrades to plain mute, which is what a lane with no project can honestly
 * answer.
 *
 * `anySoloInForce` may be passed in when the caller already computed it for
 * the whole pass (applying the rule over N lanes is otherwise N whole-tree
 * walks) — and it must come from anySoloInForce(), not anySoloInTree().
 */
inline bool isLaneAudible( SObject *root, SObject *lane, bool anySolo )
{
    if( !lane ) return false;
    if( lane->isMuted() ) return false;
    if( !anySolo ) return true;

    if( scopeOf( lane ) == SProject::SoloScope::Global ) {
        // Itself, or anything it reaches through placements: the soloed lane
        // and every container on the path down to it stay audible.
        {
            QSet<const SObject *> onPath;
            if( detail::reachesSoloedLane( lane, onPath, 0 ) ) return true;
        }
        // ...and inside a soloed ancestor, asked from the soloed lanes.
        const QList<SObject *> soloed = soloedLanesInProject( lane->getProjectSafe() );
        for( SObject *s : soloed ) {
            if( s == lane ) continue;
            QSet<const SObject *> onPath;
            if( detail::reachesObject( s, lane, onPath, 0 ) ) return true;
        }
        return false;
    }

    if( soloInSubtree( lane ) ) return true;          // itself, or a lane below it
    return underSoloedAncestor( root, lane );          // inside a soloed folder
}

inline bool isLaneAudible( SObject *root, SObject *lane )
{
    return isLaneAudible( root, lane, anySoloInForce( root ) );
}

}  // namespace ssolo

#endif  // SSOLORULES_H
