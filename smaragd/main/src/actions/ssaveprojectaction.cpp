#include "actions/ssaveprojectaction.h"
#include "sproject.h"
#include "sactionregistry.h"
#include <QDomElement>
#include <QFile>
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
