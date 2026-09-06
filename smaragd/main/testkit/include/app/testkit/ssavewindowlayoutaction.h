#ifndef _SSAVEWINDOWLAYOUTACTION_H_
#define _SSAVEWINDOWLAYOUTACTION_H_

#include "app/actions/saction.h"
#include <QString>

/**
 * `save-window-layout` — drive `SMainWindow::saveWindowLayout()` and assert
 * what it did (proposal 46 M1/M2).
 *
 * WHAT IT GATES, and what it does not. It gates that the method produces a
 * non-empty blob under BOTH `ui/windowGeometry` and `ui/windowState`, and that
 * the `--test-case` SUPPRESSION works — an unforced call from a headless run
 * must write NOTHING, because `ctest -j4` is four processes sharing one
 * smaragd.ini and a headless run's geometry describes a window that was never
 * shown. It does NOT gate the WIRING that was the actual bug (File → Exit /
 * Cmd-Q reaching this method at all): `fileExit()` now routes through
 * `close()`, whose `promptSaveUnsavedChanges()` is a modal `exec()` and whose
 * success ends the process — there is no headless route to either. That half
 * is hand-verified and the PR says so.
 *
 * IT LEAVES THE INI EXACTLY AS IT FOUND IT. The verb reads both keys first and
 * writes them back afterwards (removing them again when they were absent), so
 * a headless run cannot leave a junk geometry behind for the developer's next
 * interactive session to restore. That is why it needs no key-ownership
 * declaration in the case header the way a `set-option` case does.
 *
 *   force         "true" (default) — reach past the isTestCaseMode() refusal.
 *                 "false" — the ordinary call an automatic caller makes.
 *   expectWritten "true" / "false" — whether the call is expected to have
 *                 written. The pair (force=false, expectWritten=false) is the
 *                 suppression assertion.
 *
 * XML format:
 * <save-window-layout force="true" expectWritten="true"/>
 * <save-window-layout force="false" expectWritten="false"/>
 */
class SSaveWindowLayoutAction : public SAction
{
public:
    SSaveWindowLayoutAction() {}
    SApplyResult apply( SProject *project ) override;
    QString name() const override
    { return QStringLiteral( "save-window-layout" ); }
    QStringList knownAttributes() const override
    { return { QStringLiteral( "force" ), QStringLiteral( "expectWritten" ) }; }
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    bool force_         = true;
    bool expectWritten_ = true;
};

#endif // _SSAVEWINDOWLAYOUTACTION_H_
