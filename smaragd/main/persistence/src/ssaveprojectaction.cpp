#include "app/persistence/ssaveprojectaction.h"
#include "app/model/sproject.h"
#include "app/actions/sactionregistry.h"
#include <QDir>
#include <QDomElement>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>

SSaveProjectAction::SSaveProjectAction(const QString &path)
    : path_(path)
{
}

SApplyResult SSaveProjectAction::apply(SProject *project)
{
    if (!project || path_.isEmpty()) {
        return {false, nullptr};
    }

    // Create the parent directory when it is missing. A SCRIPTED save (the .qxa
    // harness) can legitimately target a fresh subdirectory, and without this
    // every such case has to pre-create its own -- which is invisible until the
    // case is run in a fresh clone or git worktree, where save-project then
    // fails for a purely environmental reason. File -> Save As is unaffected:
    // a file dialog cannot produce a missing parent. A path we cannot create is
    // still a rejection, so an unwritable target is not masked.
    const QDir parent = QFileInfo(path_).absoluteDir();
    if (!parent.exists() && !parent.mkpath(".")) {
        return {false, nullptr};
    }

    QFile f(path_);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return {false, nullptr};
    }

    {
        QTextStream t(&f);
        project->serialize(t);
    }
    f.close();

    // Not undoable: writing a file does not change project state.
    return {true, nullptr};
}

void SSaveProjectAction::writeXml(QDomElement &elem) const
{
    elem.setAttribute("path", path_);
}

bool SSaveProjectAction::readXml(const QDomElement &elem, int /*version*/)
{
    path_ = elem.attribute("path", "");
    return true;
}

static const bool s_reg_saveproject = (
    SActionRegistry::instance().registerType(
        QStringLiteral("save-project"),
        []{ return new SSaveProjectAction; }
    ), true
);
