#ifndef SSETLANEHIDDENACTION_H
#define SSETLANEHIDDENACTION_H

#include "app/actions/saction.h"
#include <QList>
#include <QString>

/**
 * Action: show or hide a lane in the arranger (proposal 45 AC4.2).
 *
 * WHY THIS IS MODEL STATE AND AN ACTION, rather than a per-view preference.
 * AC4.2 requires the toggle to be ONE UNDO STEP, and an undo step is an action,
 * and an action mutates the model. The consequences are deliberate and worth
 * knowing rather than discovering: showing the master lane travels with the
 * PROJECT rather than with the machine (as the metronome switch does), and two
 * arrangers open on one project agree about what is shown.
 *
 * THE DEFAULT IS PER ROLE (SObject::laneHiddenByDefault): an ordinary track is
 * shown, a SYSTEM lane is hidden. So "hidden by default" needs no minting step,
 * and the attribute serializes only when it differs from that default -- which
 * is what keeps every project file written since M1 byte-unchanged.
 *
 * The value is ABSOLUTE, never a toggle: a script that says what it wants is
 * idempotent and can be read without counting how many times it ran (the
 * `collapse-track` rule).
 *
 * Addressing is set-track-name's: `trackPath` is an index-path from the root
 * mixer, and it is the form that reaches a SYSTEM lane at all -- `$master` is
 * the SPATH_MASTER sentinel (D9), which no top-level index can express.
 *
 * Inverse: set-lane-hidden with the previous visibility. It restores the
 * EXPLICITNESS too: a lane that was on its role default before the action goes
 * back to the default rather than to a value that happens to equal it, so an
 * undo leaves the project file byte-identical to what it was.
 */
class SSetLaneHiddenAction : public SAction {
public:
    SSetLaneHiddenAction() = default;
    SSetLaneHiddenAction( const QList<int> &trackPath, bool hidden )
        : trackPath_( trackPath ), hidden_( hidden ) {}
    /// The inverse form: restore "no explicit setting" (the role default).
    SSetLaneHiddenAction( const QList<int> &trackPath, bool hidden, bool clear )
        : trackPath_( trackPath ), hidden_( hidden ), clear_( clear ) {}

    QString name() const override { return QStringLiteral("set-lane-hidden"); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QList<int> trackPath_;
    QString    pathRoot_;
    bool       hidden_ = true;
    // Inverse-only: put the lane back on its ROLE DEFAULT rather than on a
    // value equal to it, so an undo restores the file byte for byte.
    bool       clear_  = false;
};

#endif // SSETLANEHIDDENACTION_H
