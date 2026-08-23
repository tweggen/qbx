#ifndef SSAVEPROJECTACTION_H
#define SSAVEPROJECTACTION_H

#include "app/actions/saction.h"

// Action: serialize a project to a .qxp file at the given path.
// Non-undoable (writing a file is not a project mutation). Useful both for the
// File -> Save menu path and for scripted/automated round-trip tests.
class SSaveProjectAction : public SAction {
public:
    SSaveProjectAction() = default;
    explicit SSaveProjectAction(const QString &path);

    QString name() const override { return QStringLiteral("save-project"); }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

    // The file on disk now matches memory: SActionHistory::onApplied_() marks
    // the undo stack clean once apply() has succeeded. This is what makes
    // "unsaved changes" clear after a save at all — see saction.h's doc
    // comment and SMainWindow::saveToPath(), which does the equivalent thing
    // by hand for the interactive Save path (it calls this action's apply()
    // directly and never goes through SActionHistory::submit()).
    bool marksProjectClean() const override { return true; }

private:
    QString path_;
};

#endif // SSAVEPROJECTACTION_H
