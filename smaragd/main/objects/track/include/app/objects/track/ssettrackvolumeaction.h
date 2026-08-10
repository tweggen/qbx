#ifndef SSETTRACKVOLUMEACTION_H
#define SSETTRACKVOLUMEACTION_H

#include "app/actions/saction.h"
#include <QList>

// Action: set a track's volume to a given level.
// Inverse: another SSetTrackVolumeAction carrying the pre-mutation volume.
//
// The lane is addressed by an index-path from the root mixer ({2} = the 3rd
// top-level track, {0,1} = the 2nd child of the 1st), like set-track-solo /
// set-track-mute and the clip verbs. It used to be a single top-level index
// resolved with `mixer->childAt(trackIndex_)`, which made the fader of a track
// NESTED inside a folder unaddressable: the action resolved the wrong lane or
// nothing at all, and the Track Detail panel fell back to a non-undoable direct
// setVolume(). The legacy `trackIndex` attribute is still ACCEPTED on read so
// existing .qxa scripts keep working.
//
// Coalescing: consecutive volume changes on the same track merge (a drag of
// 50 events collapses to a single undo step via mergeKey()/mergeWith()).
class SSetTrackVolumeAction : public SAction {
public:
    SSetTrackVolumeAction() = default;
    SSetTrackVolumeAction(const QList<int> &trackPath, double newVolume);

    QString name() const override { return QStringLiteral("set-track-volume"); }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

    QString mergeKey() const override;
    bool mergeWith(const SAction *later) override;

private:
    QList<int> trackPath_;
    double newVolume_ = 0.0;
};

#endif // SSETTRACKVOLUMEACTION_H
