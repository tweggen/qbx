#ifndef SSETPITCHACTION_H
#define SSETPITCHACTION_H

#include "app/actions/saction.h"
#include <QList>

// Action: set a clip's transposition, in CENTS (1/100 semitone). Playback
// realises it in the grain stage (twGrainSource resamples each grain's
// content while grain spacing keeps following the stretch factor), so pitch
// and duration stay independent — a pitch edit never moves a clip edge.
//
// The value is ABSOLUTE, not a delta: undo restores an exact previous value
// even when the edit was clamped, and scripts can state the pitch outright.
// The +/- nudge gesture computes each clip's own target (current + step).
//
// Take stacks (proposal 17): pitch is a PER-TAKE parameter (invariant 1 —
// only length ops write through to all lanes), so this targets ONE take,
// selected by `take` (-1 = the active take, resolved at apply time).
class SSetPitchAction : public SAction {
public:
    SSetPitchAction() = default;
    SSetPitchAction( const QList<int> &clipPath, double cents,
                     int take = -1, bool broadcast = true );

    QString name() const override { return QStringLiteral("set-pitch"); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QList<int> clipPath_;
    double     cents_     = 0.0;
    int        take_      = -1;   // stacks only: which take is transposed
    // Edit groups: fan out to the members' corresponding clips, like every
    // other clip edit (resize/split/move).
    bool       broadcast_ = true;
};

#endif // SSETPITCHACTION_H
