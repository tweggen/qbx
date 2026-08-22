#ifndef SSOLORULES_H
#define SSOLORULES_H

#include "app/model/sobject.h"
#include "app/model/slink.h"

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
 *   audible = !muted && ( !anySoloAnywhere
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
 */
namespace ssolo {

// Solo set on `obj` itself or on any lane below it.
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

// Is solo in force anywhere in the project? A WHOLE-TREE walk: the historical
// "direct children of the mixer only" version is precisely why a nested solo
// changed nothing. `root` itself is not a lane, so it is not consulted.
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

/**
 * The rule. `root` is the project's root container (SStdMixer); a null root
 * degrades to plain mute, which is what a lane with no project can honestly
 * answer.
 *
 * `anySolo` may be passed in when the caller already computed it for the whole
 * pass (applying the rule over N lanes is otherwise N whole-tree walks).
 */
inline bool isLaneAudible( SObject *root, SObject *lane, bool anySolo )
{
    if( !lane ) return false;
    if( lane->isMuted() ) return false;
    if( !anySolo ) return true;
    if( soloInSubtree( lane ) ) return true;          // itself, or a lane below it
    return underSoloedAncestor( root, lane );          // inside a soloed folder
}

inline bool isLaneAudible( SObject *root, SObject *lane )
{
    return isLaneAudible( root, lane, anySoloInTree( root ) );
}

}  // namespace ssolo

#endif  // SSOLORULES_H
