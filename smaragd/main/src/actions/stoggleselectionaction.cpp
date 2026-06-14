#include "actions/stoggleselectionaction.h"
#include "sapplication.h"
#include "sproject.h"
#include "sselectionmanager.h"
#include "sactionregistry.h"
#include "actions/strackpath.h"
#include <QDomElement>

SToggleSelectionAction::SToggleSelectionAction(const QList<QList<int>> &paths)
    : paths_(paths)
{
}

SApplyResult SToggleSelectionAction::apply(SProject *project)
{
    if (!project) return {false, nullptr};

    SApplication &app = SApplication::app();

    // Validate all paths before applying
    SSelectionManager mgr;
    for (const QList<int> &path : paths_) {
        if (!mgr.isPathValid(path, project)) {
            return {false, nullptr};
        }
    }

    // Apply: toggle each path
    app.toggleSelectionFromPaths(paths_);

    // Inverse: toggle the same paths again (self-inverse)
    SAction *inverse = new SToggleSelectionAction(paths_);
    return {true, inverse};
}

void SToggleSelectionAction::writeXml(QDomElement &elem) const
{
    QStringList pathStrs;
    for (const QList<int> &path : paths_) {
        pathStrs << strackpath::pathToString(path);
    }
    elem.setAttribute("paths", pathStrs.join("|"));
}

bool SToggleSelectionAction::readXml(const QDomElement &elem, int /*version*/)
{
    paths_.clear();
    const QString pathsStr = elem.attribute("paths", "");
    if (pathsStr.isEmpty()) return true;

    const QStringList pathStrs = pathsStr.split("|", Qt::SkipEmptyParts);
    for (const QString &ps : pathStrs) {
        paths_ << strackpath::stringToPath(ps);
    }
    return true;
}

static const bool s_reg_toggleselection = (
    SActionRegistry::instance().registerType(
        QStringLiteral("toggle-selection"),
        []{ return new SToggleSelectionAction; }
    ), true
);
