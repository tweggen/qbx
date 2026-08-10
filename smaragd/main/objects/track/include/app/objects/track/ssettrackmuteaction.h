#ifndef SSETTRACKMUTEACTION_H
#define SSETTRACKMUTEACTION_H

#include "app/actions/saction.h"
#include <QList>

/**
 * Action: set a lane's MUTE flag (ABSOLUTE, not a toggle).
 *
 * It used to live in main/testkit (a script-only verb with no inverse), because
 * the UI wrote STrack::setMuted() directly and nothing but a .qxa case ever
 * needed to name a track. It is a first-class edit now: the mute button submits
 * it, so mute is undoable, and it sits next to set-track-solo instead of in the
 * test harness.
 *
 * Addressing: `trackPath` is an index-path from the root mixer and is the form
 * that can name a lane NESTED in a folder track. The legacy top-level-only
 * `trackIndex` is still accepted on read (five committed cases write it);
 * `trackPath` wins when both are present, and is what writeXml emits whenever
 * it is set.
 *
 * Inverse: set-track-mute with the previous value.
 */
class SSetTrackMuteAction : public SAction {
public:
    SSetTrackMuteAction() = default;
    SSetTrackMuteAction( int trackIndex, bool muted )
        : trackIndex_( trackIndex ), muted_( muted ) {}
    SSetTrackMuteAction( const QList<int> &trackPath, bool muted )
        : trackPath_( trackPath ), muted_( muted ) {}

    QString name() const override { return QStringLiteral("set-track-mute"); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    int        trackIndex_ = 0;
    QList<int> trackPath_;
    bool       muted_ = false;
};

#endif // SSETTRACKMUTEACTION_H
