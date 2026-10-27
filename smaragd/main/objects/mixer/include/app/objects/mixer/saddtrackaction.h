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
    // root is SPELLED as its own attribute here -- the qualified "Drums:0,1"
    // spelling is for the verbs that take a path.
    //
    // IT IS STORED IN SAction::pathRoot_ ALL THE SAME (QBX-249), and that is
    // not tidiness. The root used to live in a private `arrangement_` member,
    // where neither of the two funnels that carry a root could see it:
    //
    //   applyPropagatingRoot() copies pathRoot() onto every inverse, so the
    //   SRemoveTrackAction handed back below was MASTER-rooted -- one undo of
    //   an arrangement-side add-track DELETED A MASTER LANE and left the
    //   arrangement's standing (measured; QBX-249).
    //
    //   setPathRoot(), which stimeline::submitFor() stamps on every action the
    //   arranger submits, was simply ignored -- so "New track" in an
    //   arrangement tab added the lane to the MASTER, while the view then
    //   looked for it in its own mixer.
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
};

#endif // SADDTRACKACTION_H
