#ifndef SREMOVETRACKACTION_H
#define SREMOVETRACKACTION_H

#include "../saction.h"

// Action: remove a track from the mixer at a given index.
// Phase 1: Undo not implemented (used as inverse only).
class SRemoveTrackAction : public SAction {
public:
    explicit SRemoveTrackAction(int index);

    QString name() const override { return QStringLiteral("remove-track"); }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

private:
    int index_;
};

#endif // SREMOVETRACKACTION_H
