#ifndef _SASSERTUNSAVEDCHANGESACTION_H_
#define _SASSERTUNSAVEDCHANGESACTION_H_

#include "app/actions/saction.h"
#include <QString>

/**
 * `assert-unsaved-changes` — the exact predicate
 * SMainWindow::promptSaveUnsavedChanges() reads before deciding whether to
 * put up the "Unsaved work" dialog: `!QUndoStack::isClean()`.
 *
 * Exists to gate fix/editor-ui-and-shortcuts issue (d) — "Ctrl+S then Ctrl+W
 * still prompts unsaved changes" — headlessly. `QUndoStack::setClean()` was
 * never called ANYWHERE in this repository before that fix, so this
 * assertion would have read `expect="1"` forever after the first edit, no
 * matter how many times `<save-project>` ran.
 *
 *   expect = "1"   1 = unsaved changes are pending; 0 = the stack is clean
 */
class SAssertUnsavedChangesAction : public SAction
{
public:
    SAssertUnsavedChangesAction() {}
    SApplyResult apply( SProject *project ) override;
    QString name() const override
    { return QStringLiteral( "assert-unsaved-changes" ); }
    QStringList knownAttributes() const override
    { return { QStringLiteral( "expect" ) }; }
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    int expect_ = 1;
};

#endif // _SASSERTUNSAVEDCHANGESACTION_H_
