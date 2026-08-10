#ifndef SRESTORETRACKACTION_H
#define SRESTORETRACKACTION_H

#include "app/actions/saction.h"
#include <QList>

class SRemoveTrackAction;

// Inverse of SRemoveTrackAction: re-insert the track its owner is holding alive
// back under `parentPath` at `index`, restoring the whole subtree with its
// original identity. Created live as a remove's inverse — never
// serialized/registered.
//
// parentPath is an index-path from the root mixer: {} is the mixer itself (a
// top-level track), {0} is the first top-level track acting as a folder, and so
// on. Restoring has to re-attach the SAME way the removal detached, which is why
// the parent is carried rather than assumed to be the mixer.
class SRestoreTrackAction : public SAction {
public:
    SRestoreTrackAction(SRemoveTrackAction *owner,
                        const QList<int> &parentPath, int index);

    QString name() const override { return QStringLiteral("restore-track"); }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

private:
    SRemoveTrackAction *owner_;   // holds the pinned track (not owned)
    QList<int> parentPath_;
    int index_;
};

#endif // SRESTORETRACKACTION_H
