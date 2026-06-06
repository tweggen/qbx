#ifndef SSETTRACKVOLUMEACTION_H
#define SSETTRACKVOLUMEACTION_H

#include "../saction.h"

// Action: set a track's volume to a given level.
// Inverse: another SSetTrackVolumeAction carrying the pre-mutation volume.
//
// Coalescing: consecutive volume changes on the same track merge (a drag of
// 50 events collapses to a single undo step via mergeKey()/mergeWith()).
class SSetTrackVolumeAction : public SAction {
public:
    SSetTrackVolumeAction() = default;
    SSetTrackVolumeAction(int trackIdx, double newVolume);

    QString name() const override { return QStringLiteral("set-track-volume"); }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

    QString mergeKey() const override;
    bool mergeWith(const SAction *later) override;

private:
    int trackIndex_ = -1;
    double newVolume_ = 0.0;
};

#endif // SSETTRACKVOLUMEACTION_H
