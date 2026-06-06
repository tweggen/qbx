#include "actions/sloadprojectaction.h"
#include "sproject.h"
#include "sactionregistry.h"
#include "sprojectloader.h"
#include <QDomElement>

SLoadProjectAction::SLoadProjectAction(const QString &path)
    : path_(path)
{
}

SApplyResult SLoadProjectAction::apply(SProject *project)
{
    if (!project || path_.isEmpty()) {
        return {false, nullptr};
    }

    SProjectLoader loader(*project, path_);
    if (!loader.wasLoaded()) {
        return {false, nullptr};
    }

    if (loader.createObjects(*project) != 0) {
        return {false, nullptr};
    }

    // Not undoable: loading replaces the whole project; callers manage the swap.
    return {true, nullptr};
}

void SLoadProjectAction::writeXml(QDomElement &elem) const
{
    elem.setAttribute("path", path_);
}

bool SLoadProjectAction::readXml(const QDomElement &elem, int /*version*/)
{
    path_ = elem.attribute("path", "");
    return true;
}

static const bool s_reg_loadproject = (
    SActionRegistry::instance().registerType(
        QStringLiteral("load-project"),
        []{ return new SLoadProjectAction; }
    ), true
);
