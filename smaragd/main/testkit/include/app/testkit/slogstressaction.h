#ifndef SLOGSTRESSACTION_H
#define SLOGSTRESSACTION_H

#include "app/actions/saction.h"

// Testkit verb for the log dock's scale requirement (proposal 24 §8.6):
//
//   <log-stress count="500000" maxStallMs="50" timeoutMs="60000"/>
//
// Opens the log dock, emits `count` records, then pumps the event loop until
// the view has absorbed them, timing every single pump. It fails if any one
// pump exceeds maxStallMs -- that is the direct measurement of "the dock must
// not massively impact the application", because a pump that blocks is exactly
// what a user feels as a frozen UI.
//
// It also fails if the view has not caught up within timeoutMs, which catches
// the opposite failure: a drain so throttled it never converges.
//
// Deliberately NOT a screenshot assertion. SScreenshotAction grabs the root
// window (the whole desktop), so it cannot show the dock's state, and
// test-output/ is untracked -- the evidence has to be numeric.
class SLogStressAction : public SAction {
public:
    SLogStressAction() = default;

    QString name() const override { return QStringLiteral("log-stress"); }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

private:
    int count_      = 100000;
    int maxStallMs_ = 50;
    int timeoutMs_  = 60000;
};

#endif // SLOGSTRESSACTION_H
