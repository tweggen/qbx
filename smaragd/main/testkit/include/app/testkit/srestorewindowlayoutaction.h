#ifndef _SRESTOREWINDOWLAYOUTACTION_H_
#define _SRESTOREWINDOWLAYOUTACTION_H_

#include "app/actions/saction.h"
#include <QString>

/**
 * `restore-window-layout` -- seed `ui/windowGeometry`, drive
 * `SMainWindow::restoreWindowLayout()` and assert the window STATE it left
 * (QBX-294: never relaunch into full screen).
 *
 * WHAT IT GATES. The restore half of QBX-294: a stored geometry that carries
 * the full-screen flag must come back MAXIMIZED, and an ordinary maximized or
 * windowed geometry must come back as it was. It asserts the window's
 * `windowState()`, never its geometry: a `--test-case` run never shows the
 * main window, so no rectangle is real, but the state restoreGeometry() and
 * the fix give the window is set whether it is mapped or not. What it does NOT
 * gate is what the platform makes of that state at show() -- on macOS,
 * whether a maximized window gets its menu bar back -- which is hand-checked.
 *
 * THE SEED IS WRITTEN BY QT, not by this code: a scratch, never-shown QWidget
 * is put in the asked-for state and its saveGeometry() is the blob. `report`
 * is the exception: the 66-byte version-3 blob from the field report, rebuilt
 * field by field (frame 0,26 to 1279,827, maximized byte 0, full-screen byte
 * 0x04), except that its screen-width field holds the RUNNING screen's width:
 * Qt's restoreGeometry() refuses a blob whose stored width is off by more than
 * 0.8..1.25, and the report's screen was 1280 wide. window_geometry_test
 * covers the verbatim blob.
 *
 * IT LEAVES THE INI AND THE WINDOW AS IT FOUND THEM. Both keys are read first
 * (and whether they existed); `ui/windowState` is removed for the call so the
 * developer's real dock layout is never applied to the test window; both are
 * written back, or removed again, BEFORE anything is asserted. The window's
 * own geometry and state are put back the same way.
 *
 *   seed         "fullscreen" | "maximized" | "normal" | "report"
 *   expectState  "maximized" | "normal" | "fullscreen" -- "maximized" means
 *                maximized AND not full screen; "normal" means neither flag.
 *
 * XML format:
 * <restore-window-layout seed="fullscreen" expectState="maximized"/>
 */
class SRestoreWindowLayoutAction : public SAction
{
public:
    SRestoreWindowLayoutAction() {}
    SApplyResult apply( SProject *project ) override;
    QString name() const override
    { return QStringLiteral( "restore-window-layout" ); }
    QStringList knownAttributes() const override
    { return { QStringLiteral( "seed" ), QStringLiteral( "expectState" ) }; }
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString seed_;
    QString expectState_;
};

#endif // _SRESTOREWINDOWLAYOUTACTION_H_
