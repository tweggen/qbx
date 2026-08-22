#ifndef SFRAGMENTDELETECLIPACTION_H
#define SFRAGMENTDELETECLIPACTION_H

#include "app/actions/saction.h"
#include "tw/core/twfraction.h"
#include "tw/sources/twgrainparams.h"

#include <QList>
#include <QString>

// <delete-fragment-clip clip="Riff:0"/>
//
// Proposal 41 M2b/D3a follow-up (M8 gate): delete a clip that is ALREADY
// nested inside a fragment (a non-lane path container), addressed with the
// asset-name-qualified spelling M2b gave splacements::rootNamed() ("Riff:0" =
// the fragment registered as asset "Riff", child index 0).
//
// No production verb reaches this from a script. SUnplaceClipAction and
// SRemoveMidiClipAction both resolve their target through
// splacements::placementAt() -- which IS fragment-aware since M2b -- but both
// are "live-only inverses" (constructed only by undoing place-clip /
// insert-midi-clip), and neither of those two FORWARD verbs can place
// directly into a fragment (their own destination resolves through the still
// -strict laneAt()). So the cascade the proposal's author sanctioned keeping
// (deletion reaches every placement of the asset, because every placement
// shares the ONE fragment -- D2's invariant) has never been exercised by any
// reachable script path. This is that path, and nothing more: it resolves
// through the SAME splacements::placementAt()/containerAt() production code
// every other fragment-nested clip verb uses (set-clip-volume, resize-clip,
// ...), then does exactly what SUnplaceClipAction/SRemoveMidiClipAction's own
// apply() does -- delete the SLink. The cascade itself is not implemented
// here; it falls out of the object graph for free (one shared SCut/fragment,
// N placements, one child link).
//
// Audio-clip-only (an SCut over an SExternFile), matching every other
// fragment gate's fixture. Undoable: the inverse restores the deleted clip's
// content, window and grain params into the SAME fragment container --
// SFragmentRestoreClipAction below, modelled on SRestoreContainerClipAction's
// value-capture discipline (never a held/orphaned QObject).
class SFragmentDeleteClipAction : public SAction {
public:
    SFragmentDeleteClipAction() = default;

    QString name() const override
        { return QStringLiteral( "delete-fragment-clip" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QList<int> clipPath_;
};

// Live-only inverse of SFragmentDeleteClipAction. Never registered/
// serialized -- same discipline as SRestoreContainerClipAction, including
// returning {true, nullptr}: SActionUndoCommand::redo() re-applies the
// FORWARD action (the delete) rather than anything returned here.
class SFragmentRestoreClipAction : public SAction {
public:
    SFragmentRestoreClipAction() = default;
    // pathRoot is set via SAction::setPathRoot() by the caller, exactly as
    // SRestoreContainerClipAction's siblings do.
    SFragmentRestoreClipAction( const QList<int> &containerPath,
                                offset_t timePos, const QString &filePath,
                                const Fraction &srcStart, length_t cutDuration,
                                length_t loopLength,
                                const twGrainParams &grain );

    QString name() const override
        { return QStringLiteral( "restore-fragment-clip" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QList<int> containerPath_;
    offset_t   timePos_ = 0;
    QString    filePath_;
    Fraction   srcStart_ = Fraction( 0 );
    length_t   cutDuration_ = 0;
    length_t   loopLength_ = 0;
    twGrainParams grain_;
};

#endif // SFRAGMENTDELETECLIPACTION_H
