#include "app/selection/ssetselectionaction.h"
#include "app/model/sappcontext.h"
#include "app/model/sproject.h"
#include "app/selection/sselectionmanager.h"
#include "app/actions/sactionregistry.h"
#include "app/model/sobjectpath.h"
#include <QDomElement>

SSetSelectionAction::SSetSelectionAction(const QList<QList<int>> &paths)
    : paths_(paths)
{
}

SApplyResult SSetSelectionAction::apply(SProject *project)
{
    if (!project) return {false, nullptr};

    SAppContext &app = SAppContext::get();

    // Snapshot current selection for inverse
    QList<QList<int>> priorPaths = app.getCurrentSelectionPathsFor( pathRoot_ );

    // Validate all paths before applying
    SObject *root = project->getRootComponent();
    if (!root) return {false, nullptr};

    // pathRoot_ names WHICH root (master, or a named arrangement) the paths
    // below resolve against (proposal 09 D21) -- omitting it here validated
    // every arrangement-tab path against the MASTER tree instead, which only
    // ever "worked" because a fixture's arrangement happened to mirror the
    // master's shape. AC-a3 (Ctrl-A, one root per tab) is the first caller to
    // build a selection whose shape genuinely differs from the master's.
    SSelectionManager mgr;
    for (const QList<int> &path : paths_) {
        if (!mgr.isPathValid(path, project, pathRoot_)) {
            return {false, nullptr};
        }
    }

    // Apply: update selection
    app.setSelectionFromPathsFor(paths_, pathRoot_ );

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
        paths_ << strackpath::parseInto( pathRoot_, ps );
    }
    return true;
}

static const bool s_reg_setselection = (
    SActionRegistry::instance().registerType(
        QStringLiteral("set-selection"),
        []{ return new SSetSelectionAction; }
    ), true
);
