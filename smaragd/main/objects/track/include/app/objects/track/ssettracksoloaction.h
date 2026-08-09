#ifndef SSETTRACKSOLOACTION_H
#define SSETTRACKSOLOACTION_H

#include "app/actions/saction.h"
#include <QList>

/**
 * Action: set a lane's SOLO flag (ABSOLUTE, not a toggle).
 *
 * PATH-addressed (`trackPath`, an index-path from the root mixer, exactly like
 * set-edit-group and the clip verbs), because solo has to be expressible for a
 * lane NESTED inside a folder track — the case that was silently broken and had
 * no coverage at all, precisely because the UI toggle called STrack::setSolo()
 * directly and no verb existed to script it.
 *
 * Do NOT copy SSetTrackVolumeAction's `trackIndex`: that is a top-level-only
 * index and cannot name a nested lane (known bug, recorded in docs/ACTIONS.md).
 *
 * Inverse: set-track-solo with the previous value.
 */
class SSetTrackSoloAction : public SAction {
public:
    SSetTrackSoloAction() = default;
    SSetTrackSoloAction( const QList<int> &trackPath, bool solo );

    QString name() const override { return QStringLiteral("set-track-solo"); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QList<int> trackPath_;
    bool       solo_ = false;
};

#endif // SSETTRACKSOLOACTION_H
