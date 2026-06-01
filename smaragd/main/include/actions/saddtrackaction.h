#ifndef SADDTRACKACTION_H
#define SADDTRACKACTION_H

#include "../saction.h"

// Action: add a new track to the mixer at a given index.
// Inverse: SRemoveTrackAction
class SAddTrackAction : public SAction {
public:
    // index = -1 means append; otherwise insert at that index
    explicit SAddTrackAction(int index = -1);

    QString name() const override { return QStringLiteral("add-track"); }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

private:
    int index_;
};

#endif // SADDTRACKACTION_H
