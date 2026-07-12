#include "app/selection/sclearselectionaction.h"
#include "app/model/sappcontext.h"
#include "app/model/sproject.h"
#include "app/actions/sactionregistry.h"
#include "app/model/sobjectpath.h"
#include <QDomElement>

SApplyResult SClearSelectionAction::apply(SProject *project)
{
    if (!project) return {false, nullptr};

    SAppContext &app = SAppContext::get();

    // Snapshot current selection for inverse
    QList<QList<int>> priorPaths = app.getCurrentSelectionPaths();

    // Apply: clear selection
    app.setSelectionFromPaths(QList<QList<int>>());

    // Inverse: create a set-selection action via registry with prior paths
    SAction *inverse = SActionRegistry::instance().create(QStringLiteral("set-selection"));
    if (inverse) {
        QDomDocument doc;
        QDomElement elem = doc.createElement("temp");
        // Manually encode the prior paths for the inverse
        QStringList pathStrs;
        for (const QList<int> &path : priorPaths) {
            pathStrs << strackpath::pathToString(path);
        }
        elem.setAttribute("paths", pathStrs.join("|"));
        inverse->readXml(elem, 1);
    }
    return {true, inverse};
}

void SClearSelectionAction::writeXml(QDomElement &elem) const
{
    (void)elem;  // No parameters needed
}

bool SClearSelectionAction::readXml(const QDomElement &elem, int /*version*/)
{
    (void)elem;  // No parameters to read
    return true;
}

static const bool s_reg_clearselection = (
    SActionRegistry::instance().registerType(
        QStringLiteral("clear-selection"),
        []{ return new SClearSelectionAction; }
    ), true
);
