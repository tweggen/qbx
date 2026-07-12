#ifndef SADDTOSELECTIONACTION_H
#define SADDTOSELECTIONACTION_H

#include "app/actions/saction.h"
#include <QList>

/**
 * AddToSelection: Add given paths to the existing selection.
 * Inverse: RemoveFromSelectionAction with the same paths.
 */
class SAddToSelectionAction : public SAction
{
public:
    SAddToSelectionAction() = default;
    explicit SAddToSelectionAction(const QList<QList<int>> &paths);

    QString name() const override { return QStringLiteral("add-to-selection"); }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

private:
    QList<QList<int>> paths_;
};

#endif // SADDTOSELECTIONACTION_H
