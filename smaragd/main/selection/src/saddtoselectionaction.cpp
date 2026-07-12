#include "app/selection/saddtoselectionaction.h"
#include "app/selection/sremovefromselectionaction.h"
#include "app/shell/sapplication.h"
#include "app/model/sproject.h"
#include "app/selection/sselectionmanager.h"
#include "app/actions/sactionregistry.h"
#include "app/objects/track/strackpath.h"
#include <QDomElement>

SAddToSelectionAction::SAddToSelectionAction(const QList<QList<int>> &paths)
    : paths_(paths)
{
}

SApplyResult SAddToSelectionAction::apply(SProject *project)
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

    // Apply: add paths to selection
    app.addSelectionFromPaths(paths_);

    // Inverse: remove these paths
    SAction *inverse = new SRemoveFromSelectionAction(paths_);
    return {true, inverse};
}

void SAddToSelectionAction::writeXml(QDomElement &elem) const
{
    QStringList pathStrs;
    for (const QList<int> &path : paths_) {
        pathStrs << strackpath::pathToString(path);
    }
    elem.setAttribute("paths", pathStrs.join("|"));
}

bool SAddToSelectionAction::readXml(const QDomElement &elem, int /*version*/)
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

static const bool s_reg_addtoselection = (
    SActionRegistry::instance().registerType(
        QStringLiteral("add-to-selection"),
        []{ return new SAddToSelectionAction; }
    ), true
);
