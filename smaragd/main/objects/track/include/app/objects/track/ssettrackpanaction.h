#ifndef SSETTRACKPANACTION_H
#define SSETTRACKPANACTION_H

#include "app/actions/saction.h"
#include <QList>

// Action: set a track's PAN, -1.0 (hard left) .. +1.0 (hard right), 0 = centre
// (proposal 49 M2). ABSOLUTE; the inverse carries the pre-mutation value.
//
// Addressed exactly like set-track-volume: an index-path from the root mixer,
// so a nested lane resolves, and the system-lane sentinels ($master, $send0)
// resolve too. The value reaches twGainStage through STrack::onTrackPanChanged,
// post-FX and post-fader.
//
// REFUSED, and announced with a warning:
//   - on the CONDUCTOR lane, which carries no audio by construction (proposal
//     45), so a pan there is exactly the inert control proposal 49 exists to
//     remove;
//   - with a NON-ZERO value on a project whose width is not 2 (pan is defined
//     for 2 channels only, D1). 0 is always accepted, so a reset never fails.
// Master and send lanes ACCEPT it, and are heard.
//
// Coalescing: consecutive pans of the same lane merge into one undo step (a
// knob drag), keyed on the path like set-track-volume.
//
// NOT redirected to a Read lane: `self:Pan` automation is proposal 49 M3.
class SSetTrackPanAction : public SAction {
public:
    SSetTrackPanAction() = default;
    SSetTrackPanAction( const QList<int> &trackPath, double newPan );

    QString name() const override { return QStringLiteral( "set-track-pan" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override;

    QString mergeKey() const override;
    bool mergeWith( const SAction *later ) override;

private:
    QList<int> trackPath_;
    double newPan_ = 0.0;
};

#endif // SSETTRACKPANACTION_H
