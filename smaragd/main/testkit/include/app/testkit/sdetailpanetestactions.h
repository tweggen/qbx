#ifndef _SDETAILPANETESTACTIONS_H_
#define _SDETAILPANETESTACTIONS_H_

#include "app/actions/saction.h"
#include <QString>

/**
 * `assert-track-detail-layout` — THE TRACK DETAIL DOCK SURVIVES A SHORT DOCK.
 *
 * Builds the REAL Track Detail panel off screen at (`panelWidth`,
 * `panelHeight`) — the real FX strip and the real Feel Flow section inside it,
 * bound to the real track — and asserts that no widget was given less height
 * than it reports needing (`maxCrushed`) and that no two widgets in one
 * layout tree overlap (`maxOverlap`).
 *
 * WHY THIS AND NOT A SCREENSHOT. `screenshot` grabs the SCREEN's root window,
 * which is blank under QT_QPA_PLATFORM=offscreen and proves nothing about one
 * dock's paint (the standing reason recorded in main/testkit/CONTRACT.md).
 * Crushing is a GEOMETRY defect, and geometry is exactly what a headless run
 * can measure without a compositor.
 *
 * WHAT IT BITES. A layout that receives less space than its minimum does not
 * refuse — Qt distributes the shortfall and the children end up drawn on top
 * of one another. The panel got into that state because an explicit
 * `setMinimumHeight()` REPLACES a widget's layout-derived minimum rather than
 * raising it (Qt's qSmartMinSize), so a 100 px floor on the content widget and
 * another on the FX strip together claimed a ~400 px section fitted in 100 px.
 * At a `panelHeight` well under what the content needs, a healthy panel scrolls
 * (`scrollNeeded=1`) and reports crushed=0, overlap=0; the pre-fix one reported
 * neither.
 */
class SAssertTrackDetailLayoutAction : public SAction
{
public:
    SAssertTrackDetailLayoutAction() {}
    SApplyResult apply( SProject *project ) override;
    QString name() const override
    { return QStringLiteral( "assert-track-detail-layout" ); }
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString trackPath_ = QStringLiteral( "0" );
    int panelWidth_  = 320;
    int panelHeight_ = 260;
    int maxCrushed_  = 0;
    int maxOverlap_  = 0;
    QString contains_;
};

/**
 * `double-click-control` — THE RESET GESTURE, on a detail pane's control.
 *
 * Sends a real `MouseButtonDblClick` to one control of the Track Detail or
 * Clip Detail pane, built off screen and bound to the real model, so the
 * filter that runs is the production one (`sdefaultreset::onDoubleClick`) and
 * the commit that follows is the production commit — an undoable action, not a
 * model poke. `control` is
 *
 *   track-volume | clip-volume | clip-pan | clip-pitch | clip-stretch |
 *   clip-formant | clip-transpose | clip-velocity
 *
 * The clip fields read the CURRENT SELECTION, so select the clip first.
 *
 * Not undoable ITSELF (a gesture, like `fader-key`): what lands on the undo
 * stack is the action the control's own handler submitted, so `<undo count="1"/>`
 * after this verb undoes the reset.
 */
class SDoubleClickControlAction : public SAction
{
public:
    SDoubleClickControlAction() {}
    SApplyResult apply( SProject *project ) override;
    QString name() const override
    { return QStringLiteral( "double-click-control" ); }
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString control_ = QStringLiteral( "track-volume" );
    QString trackPath_ = QStringLiteral( "0" );
};

#endif // _SDETAILPANETESTACTIONS_H_
