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
// M1 IS DATA ONLY, and this action is the load-bearing example of it: it moves
// one integer on SProject and touches NOTHING else. In particular it must never
// reach STrack::setNBusses, whose shrink path is still Q_ASSERT_X( false, ... )
// — an undo of 6 -> 2 that propagated would abort the app. That is a property
// of the whole milestone, so it is pinned by instrumentation rather than by
// review: STrack counts its own setNBusses() calls and project_channels_test
// asserts the count does not move across apply + undo.
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
