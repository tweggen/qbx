#ifndef SSETPROJECTCHANNELSACTION_H
#define SSETPROJECTCHANNELSACTION_H

#include "app/actions/saction.h"

// Action: set the PROJECT's channel count (proposal 36 M1).
// Inverse: another SSetProjectChannelsAction carrying the pre-mutation count.
//
// ABSOLUTE, not a delta. The permitted widths are 1 / 2 / 4 / 6 / 8
// (SProject::isValidChannelCount); anything else is REJECTED — applied=false,
// no inverse, nothing mutated — rather than clamped, so a script that asks for
// 3 channels fails loudly instead of quietly getting 2.
//
// THIS ACTION REACHES THE GRAPH (since proposal 36 B4). It moves one integer on
// SProject and the whole signal path follows: every track's twTrackMix,
// twPluginChain, twGainStage and twRewire, the master mixer, the width of every
// page frozen afterwards, and the channel count of the next render. Undo
// restores the old width the same way; 8 -> 2 is an assignment, not a shrink.
// Gated by mc_width_change.qxa (2 -> 8 -> 2 under repeat_test).
//
// It was the opposite at M1, and the reversal is worth knowing about because
// the reason was sharp: "M1 IS DATA ONLY... it must never reach
// STrack::setNBusses, whose shrink path is still Q_ASSERT_X( false, ... ), so
// an undo of 6 -> 2 that propagated would abort the app" — except that
// Q_ASSERT_X is compiled OUT of this build (Qt defines QT_NO_DEBUG), so it
// would not have aborted; it would have returned silently and left the graph
// half-wired, which is worse (§7 trap 9). B4 deleted setNBusses and the
// per-bus instantiation with it, which is what made this action safe to
// propagate.
//
// Coalescing: consecutive changes merge (a spinbox drag is one undo step), like
// set-track-volume. There is one channel count per project, so the merge key is
// constant — no path to disambiguate.
class SSetProjectChannelsAction : public SAction {
public:
    SSetProjectChannelsAction() = default;
    explicit SSetProjectChannelsAction( int channels );

    QString name() const override { return QStringLiteral("set-project-channels"); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

    QStringList knownAttributes() const override;

    QString mergeKey() const override;
    bool mergeWith( const SAction *later ) override;

private:
    int channels_ = 2;
};

#endif // SSETPROJECTCHANNELSACTION_H
