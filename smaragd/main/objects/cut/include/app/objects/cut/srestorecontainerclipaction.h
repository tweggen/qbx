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
                                 const twGrainParams &grain );

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
};

#endif // SRESTORECONTAINERCLIPACTION_H
