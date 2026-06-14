#include "sremovefromselectionaction.h"
#include "saddtoselectionaction.h"
#include "sapplication.h"
#include "sproject.h"
#include "sselectionmanager.h"
#include "strackpath.h"
#include <QDomElement>

SRemoveFromSelectionAction::SRemoveFromSelectionAction(const QList<QList<int>> &paths)
    : paths_(paths)
{
}

SApplyResult SRemoveFromSelectionAction::apply(SProject *project)
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

    // Apply: remove paths from selection
    app.removeSelectionFromPaths(paths_);

    // Inverse: add these paths back
    SAction *inverse = new SAddToSelectionAction(paths_);
    return {true, inverse};
}

void SRemoveFromSelectionAction::writeXml(QDomElement &elem) const
{
    QStringList pathStrs;
    for (const QList<int> &path : paths_) {
        pathStrs << strackpath::pathToString(path);
    }
    elem.setAttribute("paths", pathStrs.join("|"));
}

bool SRemoveFromSelectionAction::readXml(const QDomElement &elem, int /*version*/)
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

static const bool s_reg_removefromselection = (
    SActionRegistry::instance().registerType(
        QStringLiteral("remove-from-selection"),
        []{ return new SRemoveFromSelectionAction; }
    ), true
);
