#ifndef SCLEARSELECTIONACTION_H
#define SCLEARSELECTIONACTION_H

#include "app/actions/saction.h"
#include <QList>

/**
 * ClearSelection: Deselect all items.
 * Inverse: SetSelection with the prior selection.
 */
class SClearSelectionAction : public SAction
{
public:
    SClearSelectionAction() = default;

    QString name() const override { return QStringLiteral("clear-selection"); }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;
};

#endif // SCLEARSELECTIONACTION_H
