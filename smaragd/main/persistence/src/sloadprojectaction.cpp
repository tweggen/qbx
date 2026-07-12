#include "app/persistence/sloadprojectaction.h"
#include "app/model/sproject.h"
#include "app/actions/sactionregistry.h"
#include "app/persistence/sprojectloader.h"
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

    // Suppress invalidation during project load to avoid deadlock.
    // All captures are empty on construction anyway, and worker threads
    // would race with the UI thread still deserializing objects.
    // When enableInvalidation() is called, worker threads will begin
    // recomputing all loaded cuts.
    project->disableInvalidation();

    if (loader.createObjects(*project) != 0) {
        project->enableInvalidation();  // Re-enable even on error
        return {false, nullptr};
    }

    // Loading complete: re-enable invalidation and trigger revalidation pass.
    project->enableInvalidation();

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
