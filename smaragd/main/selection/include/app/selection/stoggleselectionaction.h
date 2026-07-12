#ifndef STOGGLESELECTIONACTION_H
#define STOGGLESELECTIONACTION_H

#include "app/actions/saction.h"
#include <QList>

/**
 * ToggleSelection: Toggle each path in/out of the selection.
 * Inverse: ToggleSelection with the same paths (toggle is self-inverse).
 */
class SToggleSelectionAction : public SAction
{
public:
    SToggleSelectionAction() = default;
    explicit SToggleSelectionAction(const QList<QList<int>> &paths);

    QString name() const override { return QStringLiteral("toggle-selection"); }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

private:
    QList<QList<int>> paths_;
};

#endif // STOGGLESELECTIONACTION_H
