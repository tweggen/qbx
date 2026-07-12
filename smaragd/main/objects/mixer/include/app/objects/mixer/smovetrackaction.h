#ifndef SMOVETRACKACTION_H
#define SMOVETRACKACTION_H

#include "app/actions/saction.h"
#include <QList>

// Action: reorder a track within its current parent container (the mixer or a
// folder track) — the in-place move/reorder that complements SReparentTrackAction
// (which changes the parent). The track keeps its parent; only its index changes.
//
// The track is addressed by an index-path from the root mixer (see strackpath.h);
// toIndex is its target slot within the same parent. Undoable: the inverse moves
// it back to its exact original index.
class SMoveTrackAction : public SAction {
public:
    SMoveTrackAction() = default;
    SMoveTrackAction(const QList<int> &sourcePath, int toIndex);

    QString name() const override { return QStringLiteral("move-track"); }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

private:
    QList<int> sourcePath_;   // path TO the track being moved
    int        toIndex_ = -1; // target index within its current parent
};

#endif // SMOVETRACKACTION_H
