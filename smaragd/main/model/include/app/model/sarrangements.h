#ifndef SARRANGEMENTS_H
#define SARRANGEMENTS_H

#include "app/model/sobject.h"
#include "app/model/slink.h"
#include <QList>
#include <QPair>
#include <QSet>
#include <QString>

#include <functional>

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

/** Edges an edit is ABOUT to add, as (parent, child) pairs: a verb that moves
 *  several objects at once (dissolve-arrangement) asks about the graph it will
 *  leave, not the one it starts from, or two moves that are each harmless
 *  could close a cycle together. A walk follows them as if they were child
 *  links of `parent`. */
using PendingEdges = QList<QPair<const SObject *, SObject *> >;

namespace detail {

inline bool reachesRec( SObject *from, const SObject *to, QSet<const SObject *> &seen,
                        const PendingEdges *pending )
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
        if( reachesRec( &lk->getSObject(), to, seen, pending ) ) return true;
    }
    if( pending ) {
        for( const auto &e : *pending ) {
            if( e.first == from && reachesRec( e.second, to, seen, pending ) ) return true;
        }
    }
    return false;
}

}  // namespace detail

/**
 * THE WHOLE-GRAPH WALK MAY BE WIDER THAN CHILD LINKS (QBX-267). A NassauDigital
 * sampler's asset link (proposal 56 Q3) is an edge too: the sampler's track
 * renders the asset it links, yet a plugin chain is deliberately not a child
 * link, so the plain walk cannot see it. objects/sampler owns the walk that
 * follows those links (`ssampler::assetReachesTrack`, the check a link is made
 * under) and model may not name it, so the shell INSTALLS it here
 * (`ssamplerwiring::install`), and from then on every caller below asks THAT
 * walk: one check, at link time and at every later edit that changes
 * containment or placement. Without a hook (a unit test that never installed
 * one) the plain child-link walk answers.
 *
 * `via`, when given, receives the sampler hops the path took — "" when it
 * took none, i.e. the cycle (if any) is a plain reference cycle. Main thread.
 */
using ReachWalk = std::function<bool( SObject *from, const SObject *to,
                                      const PendingEdges *pending, QString *via )>;
void setReachWalk( ReachWalk walk );

/** True if `to` is `from` itself or reachable from it through any chain of
 *  child links (containment), content links (windowing) and — with the
 *  shell's walk installed — sampler asset links, plus `pending`. */
bool reaches( SObject *from, const SObject *to, QString *via = nullptr,
              const PendingEdges *pending = nullptr );

/**
 * True if placing `body` (an asset, or any shared object) into `destination`
 * would close a cycle — i.e. the destination is already reachable FROM the
 * body. Placing X into Y is safe exactly when Y is not inside X. The same
 * question answers a MOVE: attaching an existing object (a track, a clip)
 * under a new parent closes a cycle exactly when the parent is reachable from
 * the object.
 */
inline bool placementWouldCycle( SObject *body, SObject *destination, QString *via = nullptr,
                                 const PendingEdges *pending = nullptr )
{
    return reaches( body, destination, via, pending );
}

}  // namespace sarrangements

#endif  // SARRANGEMENTS_H
