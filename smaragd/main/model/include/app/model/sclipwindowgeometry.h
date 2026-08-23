#ifndef _SCLIPWINDOWGEOMETRY_H_
#define _SCLIPWINDOWGEOMETRY_H_

#include <QRect>

/**
 * Loop-marker grab-handle geometry, SHARED between every clip window
 * renderer (SCut's audio renderer, SMidiCut's event renderer) and the
 * arranger's hit test (SMVActualView::loopMarkerAt) -- proposal 41 M7's
 * lesson, applied here on purpose rather than rediscovered: paint and
 * hit-test drifted apart for two milestones there because they were two
 * separate implementations of "the same" geometry.
 *
 * Lives in app/model, the one layer every window renderer AND the arranger
 * both depend on. It used to live in objects/cut (as scutLoopHandleRect /
 * SCUT_LOOP_HANDLE_W, both still present there as thin forwards) -- but
 * objects/midi may not include objects/cut (docs/ARCHITECTURE.md's module
 * DAG: "objects/midi sits at the RANK of objects/cut"), so a MIDI clip's own
 * loop handles could not share the audio implementation where it lived.
 * Moving the geometry down here, rather than duplicating it in objects/midi,
 * is what keeps an audio clip's loop grip and a MIDI clip's the same size and
 * the same box by construction (fix/loop-behaviour, issue b).
 */
#define SCLIPWIN_LOOP_HANDLE_W 9

/**
 * The handle's rect for a loop-boundary divider at pixel `x` inside
 * `clipRect` (the clip's paint rect). Returns a null rect when the lane is
 * too short to show a grip -- no handle is drawn and none can be grabbed.
 *
 * The height is derived from a fixed small font INSIDE this helper rather
 * than from any painter's current font: the arranger leaves the 7pt ruler
 * font on the painter after drawing the time ruler, so an ambient-font
 * handle would be drawn at one size and hit-tested at another.
 */
QRect sClipWindowLoopHandleRect( const QRect &clipRect, int x );

#endif // _SCLIPWINDOWGEOMETRY_H_
