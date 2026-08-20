#ifndef SARRANGEMENTS_H
#define SARRANGEMENTS_H

#include "app/model/sobject.h"
#include "app/model/slink.h"
#include <QSet>

/**
 * Reachability over the object graph (proposal 09 D11).
 *
 * An asset is a window over a container, and placing one inside a container it
 * (transitively) windows would make the render pull itself: the live pull
 * recurses, and a capture build recurses before its own snapshot exists. §6 of
 * the proposal declares the reference graph ACYCLIC, and this is where that is
 * enforced.
 *
 * WHY THIS REPLACES `strackpath::isSelfOrDescendant` FOR THE PLACEMENT CHECK.
 * That helper walks `STrack` children within ONE root, so it cannot see a cycle
 * that leaves the tree it started in — "arrangement A places an asset of B, B
 * places an asset of A" is invisible to it, and detached arrangement roots make
 * that shape reachable for the first time. It is still correct for what it is
 * used for elsewhere (reparenting inside one tree) and is deliberately left
 * alone.
 *
 * WHY A PLAIN `childLinks()` WALK IS ENOUGH, which is NOT obvious and was
 * established by reading rather than assumed: every window type parents its
 * CONTENT link to itself — `SCut` (`scut.cpp`), `SMidiCut`, `STakeStack` — and
 * `SObject::childEvent` appends any `qobject_cast`-able `SLink` to
 * `childOrder_`. So a content reference IS a child link, and one walk covers
 * both containment and windowing. (This is the same property that makes
 * `invalidateRenderChainsContaining` descend through a placement into its
 * content; see proposal 09 M0.)
 *
 * Main thread only: it reads the child tree, which THREADING.md makes
 * main-thread-only.
 */
namespace sarrangements {

namespace detail {

inline bool reachesRec( SObject *from, const SObject *to, QSet<const SObject *> &seen )
{
    if( !from ) return false;
    if( from == to ) return true;
    // Memoised: a shared object linked from many places is visited once. Also
    // what makes this terminate if the graph is ALREADY cyclic — which it must
    // not be, but a guard that hangs on the state it is meant to reject would
    // be worse than one that reports it.
    if( seen.contains( from ) ) return false;
    seen.insert( from );
    for( SLink *lk : from->childLinks() ) {
        if( !lk ) continue;
        if( reachesRec( &lk->getSObject(), to, seen ) ) return true;
    }
    return false;
}

}  // namespace detail

/** True if `to` is `from` itself or reachable from it through any chain of
 *  child links (containment) or content links (windowing). */
inline bool reaches( SObject *from, const SObject *to )
{
    if( !from || !to ) return false;
    QSet<const SObject *> seen;
    return detail::reachesRec( from, to, seen );
}

/**
 * True if placing `body` (an asset, or any shared object) into `destination`
 * would close a cycle — i.e. the destination is already reachable FROM the
 * body. Placing X into Y is safe exactly when Y is not inside X.
 */
inline bool placementWouldCycle( SObject *body, SObject *destination )
{
    return reaches( body, destination );
}

}  // namespace sarrangements

#endif  // SARRANGEMENTS_H
