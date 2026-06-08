#ifndef SREMOVEASSETPLACEMENTACTION_H
#define SREMOVEASSETPLACEMENTACTION_H

#include "../saction.h"
#include "tw303aenv.h"
#include <QList>
#include <QString>
#include <QDomElement>

// Action: remove a placed asset from a track.
// Inverse: re-places the asset (SPlaceAssetAction).
// Live-only — never reconstructed from XML (only ever created as the inverse
// of SPlaceAssetAction).
class SRemoveAssetPlacementAction : public SAction {
public:
    SRemoveAssetPlacementAction() = default;
    SRemoveAssetPlacementAction(const QString &assetName,
                                const QList<int> &trackPath,
                                int clipIdx,
                                offset_t timePos);

    QString name() const override { return QStringLiteral("remove-asset-placement"); }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

private:
    QString assetName_;
    QList<int> trackPath_;
    int clipIdx_;
    offset_t timePos_;
};

#endif // SREMOVEASSETPLACEMENTACTION_H
