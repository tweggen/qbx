#include "sclearselectionaction.h"
#include "ssetselectionaction.h"
#include "sapplication.h"
#include "sproject.h"
#include <QDomElement>

SApplyResult SClearSelectionAction::apply(SProject *project)
{
    if (!project) return {false, nullptr};

    SApplication &app = SApplication::app();

    // Snapshot current selection for inverse
    QList<QList<int>> priorPaths = app.getCurrentSelectionPaths();

    // Apply: clear selection
    app.setSelectionFromPaths(QList<QList<int>>());

    // Inverse: restore prior selection
    SAction *inverse = new SSetSelectionAction(priorPaths);
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
