#ifndef SPLACEASSETACTION_H
#define SPLACEASSETACTION_H

#include "app/actions/saction.h"
#include "tw/graph/tw303aenv.h"
#include <QList>
#include <QString>
#include <QDomElement>

// Action: place a registered asset on a track at a given time position.
// The asset is a live SCut windowing a container; placing it creates a
// transparent reference (SLink to the asset body) on the destination track.
// Inverse: removes the placed asset (SRemoveAssetPlacementAction).
class SPlaceAssetAction : public SAction {
public:
    SPlaceAssetAction() = default;
    SPlaceAssetAction(const QString &assetName,
                      const QList<int> &trackPath,
                      offset_t timePos);

    QString name() const override { return QStringLiteral("place-asset"); }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

private:
    QString assetName_;
    QList<int> trackPath_;
    offset_t timePos_ = 0;
};

#endif // SPLACEASSETACTION_H
