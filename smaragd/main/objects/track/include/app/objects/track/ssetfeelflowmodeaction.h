#ifndef SSETFEELFLOWMODEACTION_H
#define SSETFEELFLOWMODEACTION_H

#include "app/actions/saction.h"
#include "app/objects/track/strack.h"

#include <QList>

// Action: set-feel-flow-mode trackPath="0" mode="adaptive|trained" (proposal
// 40 "Feel Flow" M3, design section 4.4).
//
// A plain model mutation -- like set-track-mute -- that flips
// STrack::feelFlowMode() and does NOT touch the render graph, the content
// epoch, or the audio a track produces (design section 3.2/4.4: mode is an
// ANALYSIS-side choice, consumed only by the next bounce+analyze pass, never
// by the graph itself). Undoable: the inverse is another instance carrying
// the previous mode.
//
// Choosing "trained" before learn-feel-flow has ever produced a structure is
// legal and inert: SFeelFlowTrackBounce/twGrooveBuildAspectPayloads both fall
// back to Adaptive scoring until a structure exists (see
// sfeelflowbounce.cpp's buildEffectiveGrooveParams and twgrooveaspect.cc's
// twGrooveBuildAspectPayloads), so nothing mis-scores in the meantime.
class SSetFeelFlowModeAction : public SAction {
public:
    SSetFeelFlowModeAction() = default;
    SSetFeelFlowModeAction( const QList<int> &trackPath, STrack::FeelFlowMode mode )
        : trackPath_( trackPath ), mode_( mode ) {}

    QString name() const override { return QStringLiteral( "set-feel-flow-mode" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override
    { return { QStringLiteral( "trackPath" ), QStringLiteral( "mode" ) }; }

private:
    QList<int>           trackPath_;
    STrack::FeelFlowMode mode_ = STrack::FeelFlowMode::Adaptive;
};

#endif // SSETFEELFLOWMODEACTION_H
