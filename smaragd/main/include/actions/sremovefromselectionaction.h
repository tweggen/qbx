#ifndef SREMOVEFROMSELECTIONACTION_H
#define SREMOVEFROMSELECTIONACTION_H

#include "../saction.h"
#include <QList>

/**
 * RemoveFromSelection: Remove given paths from the selection.
 * Inverse: AddToSelectionAction with the same paths.
 */
class SRemoveFromSelectionAction : public SAction
{
public:
    SRemoveFromSelectionAction() = default;
    explicit SRemoveFromSelectionAction(const QList<QList<int>> &paths);

    QString name() const override { return QStringLiteral("remove-from-selection"); }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

private:
    QList<QList<int>> paths_;
};

#endif // SREMOVEFROMSELECTIONACTION_H
