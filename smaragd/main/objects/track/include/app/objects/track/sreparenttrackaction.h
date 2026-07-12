#ifndef SREPARENTTRACKACTION_H
#define SREPARENTTRACKACTION_H

#include "app/actions/saction.h"
#include <QList>

// Action: move a track to a different parent container, building the track tree
// that proposal 05 (track grouping) is founded on. A "container" is the root
// SStdMixer or any STrack — both hold their children as SLink objects, so a
// track summed by a parent track's twTrackMix becomes a group member.
//
// Tracks are identified by a *path*: a chain of child indices from the root
// mixer. [] is the mixer itself; [2] is its third child; [2,1] is the second
// child of that child. The source path points AT the track to move; the
// destination path points AT the parent container to move it under. destIndex
// is the exact slot within the destination (-1 = append).
//
// Inverse: another SReparentTrackAction moving the track back to its exact
// original (parent, index). It is synthesized from the post-move tree so it
// survives the index shifts the move causes.
//
// Notes:
//  - Same-container moves (pure reorder) are rejected — use SMoveTrackAction.
//  - A cycle guard refuses to move a track into itself or a descendant.
class SReparentTrackAction : public SAction {
public:
    SReparentTrackAction() = default;
    SReparentTrackAction(const QList<int> &sourcePath,
                         const QList<int> &destParentPath,
                         int destIndex = -1);

    QString name() const override { return QStringLiteral("reparent-track"); }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

private:
    QList<int> sourcePath_;       // path TO the track being moved
    QList<int> destParentPath_;   // path TO the new parent container
    int        destIndex_ = -1;   // advisory insertion index (append-only today)
};

#endif // SREPARENTTRACKACTION_H
