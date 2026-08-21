#ifndef SPLAYHEADMAP_H
#define SPLAYHEADMAP_H

#include "app/model/sobject.h"
#include "app/model/slink.h"
#include "app/model/sproject.h"
#include <QSet>
#include <QString>

/**
 * Where an ARRANGEMENT is being heard (proposal 09 §15).
 *
 * The transport has exactly ONE position, and it is a frame of the MASTER
 * timeline. An arrangement root has a timeline of its own that the transport
 * knows nothing about, so a tab showing one cannot draw the global locator —
 * that number means nothing in its domain, and drawing it there was the
 * playback-position inconsistency this header exists to remove.
 *
 * What it can draw is DERIVED: the master position walked DOWN through
 * whatever places that arrangement. If a drum loop sits at bar 5 of the
 * master, then while the master plays bars 1-4 the arrangement is not being
 * heard at all, and from bar 5 on its cursor cycles exactly as the clip's
 * loop cycles — which is the behaviour a user expects and the reason the walk
 * goes through SObject::windowStep() rather than subtracting a start time.
 *
 * THREE THINGS THAT ARE DECISIONS, NOT DETAILS:
 *
 * 1. WHICH PLACEMENT WINS. An arrangement may be placed any number of times,
 *    and two placements can overlap in the master timeline (different lanes,
 *    or a lane holding overlapping clips). The FIRST hit in child-link order
 *    wins — deterministic, and the same order the arranger draws in. It is a
 *    genuine choice: the alternative, N cursors in one tab, is what proposal
 *    09 §4 rejected when it decided a drawn-from tab needed no playhead at
 *    all. One cursor that is right for one placement beats N that the user
 *    must disambiguate; the tab is showing ONE timeline.
 * 2. NOT FOUND IS A REAL ANSWER, not a zero. `sounding` false means the
 *    arrangement is not being heard at this instant — because the locator is
 *    outside every placement, because the placement is on an inactive take, or
 *    because nothing places it at all. The caller decides what to draw; it must
 *    not read `pos` as a position.
 * 3. THE GUARD IS PER PATH, not a global memo. The same body legitimately
 *    reached twice at two positions must be visited twice: the first reach may
 *    not contain the position while the second does. A global `seen` would
 *    silently answer "not sounding" for the second. What it guards is a CYCLE,
 *    which sarrangements::placementWouldCycle refuses at the edit — this is the
 *    backstop for a file that arrived cyclic anyway, and a cycle here is a
 *    stack overflow on the UI thread, not a wrong pixel.
 *
 * Main thread only (it reads the child tree), and NON-BLOCKING: a repaint calls
 * it, so everything it touches — getDuration(), windowStep() — is contractually
 * try-lock or pure. See main/timeline/CONTRACT.md inv. 1.
 */
namespace splayhead {

/** The answer for one arrangement at one master position. */
struct Position {
    bool     sounding = false;   ///< is this arrangement being heard right now?
    offset_t pos      = 0;       ///< only meaningful when `sounding`
};

// A cycle in the reference graph is a stack overflow, not a wrong answer, so
// depth is bounded as well as the path set. Deeper than this is a project no
// gesture in the app can build.
static const int kMaxDepth = 32;

namespace detail {

inline bool findRec( SProject *project, SObject *obj, offset_t pos,
                     const QString &wanted, QSet<const SObject *> &onPath,
                     int depth, offset_t &out )
{
    if( !obj || depth > kMaxDepth ) return false;
    if( onPath.contains( obj ) ) return false;      // cycle backstop (note 3)

    if( project->arrangementNameOf( obj ) == wanted ) { out = pos; return true; }

    onPath.insert( obj );
    bool found = false;

    // A window (a cut, a take column) maps the position itself: slip, loop
    // fold and stretch all live there, and only it can apply them.
    SObject::SWindowStep step;
    if( obj->windowStep( pos, step ) ) {
        found = findRec( project, step.content, step.pos, wanted,
                         onPath, depth + 1, out );
    } else {
        for( SLink *lk : obj->childLinks() ) {
            if( !lk ) continue;
            SObject &child = lk->getSObject();
            const offset_t rel = pos - lk->getStartTime();
            if( child.isLane() ) {
                // A lane or a folder: no end, so no span test. Its own start
                // time still shifts the domain.
                found = findRec( project, &child, rel, wanted,
                                 onPath, depth + 1, out );
            } else {
                // A placement: audible only across its own span. This is what
                // makes "the arrangement is not being heard yet" an answer.
                const length_t dur = child.getDuration();
                if( rel < 0 || rel >= (offset_t) dur ) continue;
                found = findRec( project, &child, rel, wanted,
                                 onPath, depth + 1, out );
            }
            if( found ) break;                      // first hit wins (note 1)
        }
    }

    onPath.remove( obj );
    return found;
}

}  // namespace detail

/**
 * Where `rootName` is being heard when the master locator is at `masterPos`.
 * An EMPTY name is the master itself — the identity, which is why a master
 * tab pays nothing for any of this.
 */
inline Position derivedPos( SProject *project, const QString &rootName,
                            offset_t masterPos )
{
    Position r;
    if( rootName.isEmpty() ) { r.sounding = true; r.pos = masterPos; return r; }
    if( !project ) return r;
    SObject *master = project->getRootComponent();
    if( !master ) return r;
    QSet<const SObject *> onPath;
    offset_t found = 0;
    if( detail::findRec( project, master, masterPos, rootName, onPath, 0, found ) ) {
        r.sounding = true;
        r.pos = found;
    }
    return r;
}

}  // namespace splayhead

#endif  // SPLAYHEADMAP_H
