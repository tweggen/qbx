#ifndef SLEARNFEELFLOWACTION_H
#define SLEARNFEELFLOWACTION_H

#include "app/actions/saction.h"

#include <QList>
#include <cstdint>
#include <vector>

// Action: learn-feel-flow (proposal 40 "Feel Flow" M3, design section 4.4's
// "learn-from-selection" control).
//
// FORWARD form (the ordinary XML spelling, `<learn-feel-flow trackPath=.../>`):
// fits a FROZEN groove structure (tw/sidecar/twgroovependulum.h's
// twGroovePendulumTrainStructure -- the exact engine API M0's fixture-(i) gate
// exercises) from the project's CURRENT in/out selection
// (SProjectProps::RangeStart/RangeEnd/RangeValid -- the same one range the
// render dialog's "time selection" and record_punch's punch region already
// read), over the addressed track's own bounce audio (STrack::
// feelFlowBouncePath() -- the SAME post-FX signal the overlay/panel analyze,
// sliced to the selection). Requires the track to have been bounced at least
// once (STrack::feelFlowHasResult()) and a valid selection to be set;
// training is SECTION-SCOPED by construction, never over the whole track,
// because material that legitimately changes feel per section must be
// trained per range (design section 4.4, review finding 3.4).
//
// RESTORE form (`restore="1"`, undo/redo only -- never written by a script):
// replaces the track's trained structure VERBATIM with the one carried in
// `data` (base64, twGrooveTrainedStructureSerialize's wire format; ABSENT
// `data` means "no prior structure", i.e. clear). apply()'s own inverse is
// always a restore-form instance: undo/redo must reproduce the EXACT
// pre-mutation state, never re-run a fit that depends on the project's
// CURRENT selection and bounce audio (which may have moved since).
//
// Non-undoable ONLY in the sense that it does not touch the render graph --
// it IS undoable in the ordinary sense (the model mutation it makes has a
// working inverse), unlike feel-flow-analyze which schedules a background
// job and returns a null inverse.
class SLearnFeelFlowAction : public SAction {
public:
    SLearnFeelFlowAction() = default;
    explicit SLearnFeelFlowAction( const QList<int> &trackPath )
        : trackPath_( trackPath ) {}

    QString name() const override { return QStringLiteral( "learn-feel-flow" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override;

    // Exposed for STrack/testkit callers building the inverse directly
    // (mirrors how other actions in this module are constructed for their
    // undo entries) -- never used by readXml's forward form.
    void setRestore( bool hadPrior, const std::vector<uint8_t> &blob )
    { restoreIsSet_ = true; restoreHadPrior_ = hadPrior; restoreBlob_ = blob; }

private:
    QList<int> trackPath_;

    bool                  restoreIsSet_    = false;
    bool                  restoreHadPrior_ = false;   // false + restoreIsSet_ = "clear"
    std::vector<uint8_t>  restoreBlob_;
};

#endif // SLEARNFEELFLOWACTION_H
