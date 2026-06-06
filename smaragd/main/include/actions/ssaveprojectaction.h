#ifndef SSAVEPROJECTACTION_H
#define SSAVEPROJECTACTION_H

#include "../saction.h"

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

private:
    QString path_;
};

#endif // SSAVEPROJECTACTION_H
