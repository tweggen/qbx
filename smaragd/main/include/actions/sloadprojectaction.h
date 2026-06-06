#ifndef SLOADPROJECTACTION_H
#define SLOADPROJECTACTION_H

#include "../saction.h"

// Action: load a .qxp file INTO the supplied project, building its object tree.
// The target project should be empty (e.g. a freshly constructed SProject);
// callers swap it in as the current project afterwards.
//
// Non-undoable. Used by the File -> Open menu path and by round-trip tests.
class SLoadProjectAction : public SAction {
public:
    SLoadProjectAction() = default;
    explicit SLoadProjectAction(const QString &path);

    QString name() const override { return QStringLiteral("load-project"); }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

private:
    QString path_;
};

#endif // SLOADPROJECTACTION_H
