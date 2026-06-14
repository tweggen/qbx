#ifndef SSETSELECTIONACTION_H
#define SSETSELECTIONACTION_H

#include "../saction.h"
#include <QList>

/**
 * SetSelection: Clear all selection and set to the given list of paths.
 * Inverse: SetSelection with the prior selection.
 */
class SSetSelectionAction : public SAction
{
public:
    SSetSelectionAction() = default;
    explicit SSetSelectionAction(const QList<QList<int>> &paths);

    QString name() const override { return QStringLiteral("set-selection"); }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

private:
    QList<QList<int>> paths_;
};

#endif // SSETSELECTIONACTION_H
