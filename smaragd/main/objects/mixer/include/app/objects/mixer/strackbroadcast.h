#ifndef STRACKBROADCAST_H
#define STRACKBROADCAST_H

#include <QList>

class SStdMixer;
class STrack;

/**
 * WHICH TRACKS A GESTURE ACTS ON (proposal 48 D3, M0).
 *
 * The arranger's track head commits mute, solo, arm, edit-group, track input,
 * monitor mode and automation mode OVER THE SELECTION, as one undo macro.  The
 * rule that decides the target list is:
 *
 *     only a gesture aimed INTO the selection broadcasts; a gesture aimed at a
 *     lane outside it acts on that lane alone.
 *
 * A second mount that toggled only the strip clicked would be a SECOND rule,
 * and the user would find out which one they touched by undoing.  So the
 * target COMPUTATION lives here, once.
 *
 * WHAT DELIBERATELY DOES NOT EXTRACT: THE SUBMIT.  Every existing commit goes
 * through `stimeline::submitActive` or builds a macro on
 * `SApplication::app().actionHistory()->undoStack()`, and `objects/mixer` can
 * reach neither -- `SAppContext` exposes no submit and no undo stack, which is
 * the deliberate minimalism `docs/ARCHITECTURE.md` asks for.  Putting a
 * `submitOverSelection` down here would mean widening `SAppContext` for one
 * convenience.  The macro shape is three lines at each mount and is duplicated
 * ON PURPOSE; what may never be duplicated is what is in this header.
 */
namespace strackbroadcast {

/// D3's rule.  `clicked` alone unless it is part of a multi-track selection,
/// in which case the whole selection, in lane order.
QList<STrack *> targetsFor( SStdMixer *mixer, STrack *clicked );

/// Sort into the order the lanes appear in (`slaneorder::flattenTrackLanes`
/// with the ARRANGER's options), stably.
///
/// **A TRACK WITH NO VISIBLE LANE SORTS LAST, and that is PRESERVED rather
/// than inherited by accident** (proposal 48 AC0.4 / D3a).  Before M0 this
/// read `rowIndexOfTrack()`, which answers -1 for a track that has no row --
/// so the "sorts last" behaviour was an artifact of the view's row list rather
/// than a decision.  It is reproduced here EXACTLY, because the row list IS
/// the flattened walk with fold and hidden honoured: a track absent from that
/// walk is precisely a track with no row.  Nothing in the qxa suite covers a
/// multi-selection spanning a COLLAPSED folder -- the one shape that can
/// distinguish the two orders -- so `laneorder_test` asserts it instead.
///
/// It is load-bearing for `newTrackReference_()` (which takes `sel.last()`)
/// and for the top-down / bottom-up loops in indent and outdent.
QList<STrack *> orderByLane( SStdMixer *mixer, const QList<STrack *> &in );

/// Drop any track that is a descendant of another in the list, so a STRUCTURAL
/// verb (remove, indent, outdent, group, asset) acts on each subtree once.
/// Pure -- no mixer needed.
QList<STrack *> pruneNestedTargets( const QList<STrack *> &in );

}  // namespace strackbroadcast

#endif // STRACKBROADCAST_H
