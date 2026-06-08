#ifndef SCREATEASSETACTION_H
#define SCREATEASSETACTION_H

#include "../saction.h"
#include "tw303aenv.h"
#include <QList>
#include <QString>

// Action: create a live "asset" — an SCut that windows an existing container
// (the root mixer or a folder track), bounded *vertically* by that container
// and *horizontally* by [startOffset, startOffset+duration) — and register it
// as a named project resource. The asset is a shared SObject pulled live, so
// editing the container changes every placement of the asset.
//
// The container is addressed by an index-path from the root mixer ([] = the
// mixer itself, {2} = its 3rd child track). Inverse: SRemoveAssetAction.
// See plan/proposed/05_TRACK_GROUPING_AND_LIVE_ASSETS.md feature (b), §4b.
class SCreateAssetAction : public SAction {
public:
    SCreateAssetAction() = default;
    SCreateAssetAction( const QList<int> &containerPath,
                        offset_t startOffset, length_t duration,
                        const QString &assetName = QString() );

    QString name() const override { return QStringLiteral("create-asset"); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QList<int> containerPath_;
    offset_t   startOffset_ = 0;
    length_t   duration_    = 0;
    QString    assetName_;   // empty => generate a unique one at apply()
};

#endif // SCREATEASSETACTION_H
