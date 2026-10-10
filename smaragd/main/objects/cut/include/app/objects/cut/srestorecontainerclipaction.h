#ifndef SRESTORECONTAINERCLIPACTION_H
#define SRESTORECONTAINERCLIPACTION_H

#include "app/actions/saction.h"
#include "tw/core/twfraction.h"
#include "tw/sources/twgrainparams.h"

#include <QList>

// Undo of "delete a clip that windows a CONTAINER" — an asset copy, i.e. a cut
// whose content is a track rather than a wave.
//
// Two kinds of container-backed clip get deleted, and they need different
// inverses:
//   - the clip IS a registered asset body (a placement links the body itself).
//     Undo must RE-PLACE it so the asset keeps its identity — that is
//     SPlaceAssetAction, reached via SRemoveAssetPlacementAction, and it lives
//     in the mixer slice with the rest of the asset actions.
//   - the clip is a COPY of one (duplicated, then re-pitched, say). Nothing in
//     the registry points at it, so undo has to rebuild the cut: same content
//     container, same window. That is this action.
//
// Content is addressed by index path from the root container, so the action
// holds no raw pointer into the object tree.
//
// Live-only, like SRemoveAssetPlacementAction: it is manufactured as an inverse
// and never appears in a script, so it is neither serialized nor registered.
class SRestoreContainerClipAction : public SAction {
public:
    SRestoreContainerClipAction() = default;
    SRestoreContainerClipAction( const QList<int> &lanePath,
                                 const QList<int> &containerPath,
                                 offset_t timePos,
                                 const Fraction &srcStart,
                                 length_t cutDuration,
                                 length_t loopLength,
                                 const twGrainParams &grain,
                                 int restoreAtIndex = -1 );

    QString name() const override
        { return QStringLiteral("restore-container-clip"); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QList<int> lanePath_;        // where the clip is placed
    QList<int> containerPath_;   // what the cut windows
    offset_t timePos_ = 0;
    Fraction srcStart_ = Fraction(0);
    length_t cutDuration_ = 0;
    length_t loopLength_ = 0;
    twGrainParams grain_;

    // Where in the lane's child order to put the restored clip, or -1 to
    // append (QBX-149; see saddsampleaction.h for the full reasoning). A clip is addressed by its POSITIONAL index, so an
    // inverse that appends leaves every stored index meaning a different clip:
    // delete the clip at index 0, undo, and the restored clip lands last, so
    // redoing `remove-sample(clipIndex=0)` deletes a DIFFERENT clip. Measured
    // before the fix: delete head, undo, redo, and the head survived while the
    // tail was destroyed.
    //
    // Position is the right thing to restore here rather than switching to
    // identity addressing: childIndex is this project's sanctioned ordering
    // key (an SObject's id is its memory address and must never order
    // anything), so the order IS part of the state an inverse owes the user.
    // Not serialized, unlike SAddSampleAction's copy of this field: this
    // action is live-only by design (writeXml is intentionally empty and
    // readXml returns false), so there is no XML for it to round-trip through.
    int restoreAtIndex_ = -1;
};

#endif // SRESTORECONTAINERCLIPACTION_H
