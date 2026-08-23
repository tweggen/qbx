#ifndef _SCLIPCOLORS_H
#define _SCLIPCOLORS_H

#include <QColor>
#include <QString>

class SObject;

/**
 * THE ONE AUTHORITY ON WHAT COLOUR A CLIP IS.
 *
 * A track carries a colour INDEX into a fixed palette of 16 named anchors, and
 * every other colour a clip needs -- the selected body, the muted body, the
 * waveform on it -- is DERIVED from that one anchor rather than stored. Three
 * consequences, and the third is the reason this file exists at all:
 *
 *   1. A user-chosen colour (later: a picker writing SObject::colorIndex_)
 *      gets its whole family for free. Nothing has to invent a matching
 *      "bright" or "muted" variant per feature.
 *   2. The palette is 16 entries, not 64.
 *   3. THE PIXEL GATES READ THE SAME FUNCTION THE PAINT PATH DOES.
 *      `assert-lane-overlay` and `assert-take-lane` classify a grabbed lane by
 *      exact colour and by luminance BAND, and they used to hardcode
 *      QColor(160,160,160) as "the clip body" -- true only while every clip in
 *      the project was the same grey. The same discipline proposal 41 M7
 *      arrived at for tagChipRect(): one function, shared by the painter and
 *      the measurement, so the two cannot drift.
 *
 * It lives in app/model -- the LOWEST layer -- because everything that needs a
 * clip colour is above it and none of those may see each other: app/objects/
 * track paints the composite lane, app/objects/wave and app/objects/midi paint
 * the content on it, app/timeline paints the take lanes, and app/shell
 * measures all three.
 *
 * THE ANCHORS ARE MID-TONE AND QUIET ON PURPOSE (S 20-44 %, L 36-48 %): the
 * SELECTED variant is the same hue made lighter, so an anchor that already sat
 * near white would have nowhere to go and selection would stop reading.
 * Neighbouring indices are far apart in HUE rather than adjacent on the wheel:
 * the auto assignment is by lane order, and a 22.5-degree step between two
 * lanes stacked on top of each other is not a distinction anyone can see.
 */
namespace sclipcolors {

/** Number of palette anchors. Every index is taken modulo this. */
int count();

/** The human name of an anchor ("Denim", "Clay", ...) -- for a future picker. */
QString name( int index );

/** The anchor itself: the body colour of an unselected, unmuted clip. */
QColor base( int index );

/**
 * The clip BODY for one state. Selected wins over muted: a selected muted clip
 * is drawn as the thing the user is pointing at, because the mute is already
 * said by the track head and the selection is not said anywhere else once the
 * white border is gone.
 */
QColor body( int index, bool selected, bool muted );

/**
 * The WAVEFORM (and, on an event clip, the note) drawn on that body: the same
 * hue, nearly desaturated and much lighter -- ~#CCC unselected, ~#FFF selected
 * -- so the content reads as content at any anchor and never competes with the
 * body for hue.
 */
QColor wave( int index, bool selected, bool muted );

/**
 * The AUTO index of a lane: its position in the project's flattened lane
 * order, counting lanes only (a folder's children follow it, exactly as their
 * rows do). Returns 0 when the lane is not reachable from `root`, so a colour
 * is always available -- a lane that cannot be found is a bug elsewhere, and a
 * clip painted in anchor 0 is a far better outcome than an invisible one.
 *
 * O(number of lanes). Resolve it ONCE per lane paint, not once per clip.
 */
int autoIndexForLane( SObject &root, const SObject &lane );

/** `lane.colorIndex()` when the user has chosen one, else autoIndexForLane(). */
int indexForLane( SObject &root, const SObject &lane );

}  // namespace sclipcolors

/**
 * The resolved pair for ONE clip, carried down the render context so the
 * content renderers (a wave, a cut, an event clip) draw in their track's
 * colour without any of them knowing what a track is. Set by whoever fills the
 * body -- the composite lane's clip loop and the take lanes -- immediately
 * before the child renderer is called.
 *
 * The defaults are the PRE-PALETTE colours on purpose: a context built by
 * something that never sets them (a unit test's scratch context) still paints
 * a visible clip rather than an invisible one.
 */
struct SClipColors
{
    QColor body { 160, 160, 160 };
    QColor wave { 200, 200, 200 };
};

#endif
