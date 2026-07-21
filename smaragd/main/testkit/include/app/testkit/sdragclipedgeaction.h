#ifndef SDRAGCLIPEDGEACTION_H
#define SDRAGCLIPEDGEACTION_H

#include "app/actions/saction.h"
#include "tw/graph/tw303aenv.h"
#include <Qt>

/**
 * Test action: perform a clip-edge DRAG through the arranger's real mouse
 * handlers (press → move → release), the way a user does it.
 *
 * Why this exists: every clip-edge clamp and snap lives in
 * SMVActualView::mouseMoveEvent. The model-level `resize-clip` action bypasses
 * all of it, so a script built from `resize-clip` cannot see a gesture bug —
 * it will happily set a duration the UI refuses to produce. This action is the
 * only way the qxa suite reaches trim / extend / loop / loop-marker behaviour.
 *
 * Not undoable itself: the gesture it drives submits its own undoable action
 * (SResizeClipAction) on release, exactly as the interactive drag does.
 *
 * XML format:
 * <drag-clip-edge track="0" clip="0" edge="end" toTime="768000" half="lower"
 *                  modifiers="alt"/>
 *
 * Parameters:
 * - track:  lane row index in the flattened track tree (default 0)
 * - clip:   index among the lane's real clips, nested lanes skipped (default 0)
 * - edge:   "end" (right edge), "start" (left edge), or "body" — the body
 *           grab is what slip / duplicate / move need, since a press inside an
 *           edge band can never arm them                        (default "end")
 * - toTime: drop position, in frames
 * - half:   "lower" = extend/trim, "upper" = the loop half of the edge band
 *                                                                (default "lower")
 * - modifiers: "+"-separated list of "ctrl", "alt", "shift"       (default none).
 *           Delivered ON THE EVENT, so the modifier gestures are drivable:
 *           alt on the body = slip, ctrl on a border = time-stretch, ctrl on
 *           the body = duplicate.
 *
 * Limit: the drop lands on a pixel boundary, so the resulting time is quantised
 * to the view's current zoom; assert on ranges, not exact frame counts.
 */
class SDragClipEdgeAction : public SAction {
public:
    SDragClipEdgeAction() = default;
    SDragClipEdgeAction( int track, int clip, int grabWhere, offset_t toTime,
                         bool upperHalf, Qt::KeyboardModifiers mods = Qt::NoModifier );

    QString name() const override { return QStringLiteral("drag-clip-edge"); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    int      track_     = 0;
    int      clip_      = 0;
    int      grabWhere_ = 1;   // 0 = start edge, 1 = end edge, 2 = body
    offset_t toTime_    = 0;
    bool     upperHalf_ = false;
    Qt::KeyboardModifiers mods_ = Qt::NoModifier;
};

#endif
