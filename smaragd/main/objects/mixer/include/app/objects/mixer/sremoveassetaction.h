#ifndef SREMOVEASSETACTION_H
#define SREMOVEASSETACTION_H

#include "app/actions/saction.h"
#include <QString>

// Action: remove a live asset from the project's asset registry (the inverse of
// SCreateAssetAction). The asset body is a derived SCut (container reference +
// window), so removal just unregisters it — dropping the pinned reference lets
// it fall away — and the inverse re-creates an identical one from the captured
// container path + window + name, so undo/redo round-trips.
// See plan/proposed/05_TRACK_GROUPING_AND_LIVE_ASSETS.md feature (b).
class SRemoveAssetAction : public SAction {
public:
    SRemoveAssetAction() = default;
    explicit SRemoveAssetAction( const QString &assetName );

    QString name() const override { return QStringLiteral("remove-asset"); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString assetName_;
};

#endif // SREMOVEASSETACTION_H
