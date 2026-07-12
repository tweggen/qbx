#include "app/selection/ssetselectionaction.h"
#include "app/shell/sapplication.h"
#include "app/model/sproject.h"
#include "app/selection/sselectionmanager.h"
#include "app/actions/sactionregistry.h"
#include "app/objects/track/strackpath.h"
#include <QDomElement>

SSetSelectionAction::SSetSelectionAction(const QList<QList<int>> &paths)
    : paths_(paths)
{
}

SApplyResult SSetSelectionAction::apply(SProject *project)
{
    if (!project) return {false, nullptr};

    SApplication &app = SApplication::app();

    // Snapshot current selection for inverse
    QList<QList<int>> priorPaths = app.getCurrentSelectionPaths();

    // Validate all paths before applying
    SObject *root = project->getRootComponent();
    if (!root) return {false, nullptr};

    SSelectionManager mgr;
    for (const QList<int> &path : paths_) {
        if (!mgr.isPathValid(path, project)) {
            return {false, nullptr};
        }
    }

    // Apply: update selection
    app.setSelectionFromPaths(paths_);

    // Inverse: restore prior selection
    SAction *inverse = new SSetSelectionAction(priorPaths);
    return {true, inverse};
}

void SSetSelectionAction::writeXml(QDomElement &elem) const
{
    QStringList pathStrs;
    for (const QList<int> &path : paths_) {
        pathStrs << strackpath::pathToString(path);
    }
    elem.setAttribute("paths", pathStrs.join("|"));
}

bool SSetSelectionAction::readXml(const QDomElement &elem, int /*version*/)
{
    paths_.clear();
    const QString pathsStr = elem.attribute("paths", "");
    if (pathsStr.isEmpty()) return true;  // Empty selection is valid

    const QStringList pathStrs = pathsStr.split("|", Qt::SkipEmptyParts);
    for (const QString &ps : pathStrs) {
        paths_ << strackpath::stringToPath(ps);
    }
    return true;
}

static const bool s_reg_setselection = (
    SActionRegistry::instance().registerType(
        QStringLiteral("set-selection"),
        []{ return new SSetSelectionAction; }
    ), true
);
