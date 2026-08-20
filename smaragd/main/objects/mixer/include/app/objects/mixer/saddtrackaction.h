#ifndef SADDTRACKACTION_H
#define SADDTRACKACTION_H

#include "app/actions/saction.h"

// Action: add a new track to the mixer at a given index.
// Inverse: SRemoveTrackAction
class SAddTrackAction : public SAction {
public:
    // index = -1 means append; otherwise insert at that index.
    // arrangement = "" is the MASTER root; a name addresses that arrangement
    // (proposal 09 D21). add-track takes an INDEX rather than a path, so the
    // root travels as its own attribute here -- the qualified "Drums:0,1"
    // spelling is for the verbs that take a path.
    explicit SAddTrackAction(int index = -1,
                             const QString &arrangement = QString());

    QString name() const override { return QStringLiteral("add-track"); }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;
    QStringList knownAttributes() const override
    { return { QStringLiteral("index"), QStringLiteral("arrangement") }; }

private:
    int     index_;
    QString arrangement_;
};

#endif // SADDTRACKACTION_H
