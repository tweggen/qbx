#ifndef SRESTORETRACKACTION_H
#define SRESTORETRACKACTION_H

#include "app/actions/saction.h"

class SRemoveTrackAction;

// Inverse of SRemoveTrackAction: re-insert the track its owner is holding alive
// back into the mixer at `index`, restoring the whole subtree with its original
// identity. Created live as a remove's inverse — never serialized/registered.
class SRestoreTrackAction : public SAction {
public:
    SRestoreTrackAction(SRemoveTrackAction *owner, int index);

    QString name() const override { return QStringLiteral("restore-track"); }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

private:
    SRemoveTrackAction *owner_;   // holds the pinned track (not owned)
    int index_;
};

#endif // SRESTORETRACKACTION_H
