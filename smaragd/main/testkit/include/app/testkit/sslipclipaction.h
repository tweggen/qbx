#ifndef SSLIPCLIPACTION_H
#define SSLIPCLIPACTION_H

#include "app/actions/saction.h"
#include "tw/core/twtypes.h"
#include "tw/core/twfraction.h"

#include <QList>

// <slip-clip clip="0,0" startOffset="48000"/>
// <slip-clip clip="0,0" srcStart="48000"/>
// <slip-clip clip="0,0" loopStart="24000"/>
//
// ONE tick of a live slip drag: calls SCut::setStartOffset / setSrcStart /
// setLoopStart directly, exactly as SStdMixerView's alt-drag-the-body gesture
// does on every mouse-move, and does NOT submit an SResizeClipAction.
//
// That distinction is the whole point of the verb. The commit at the end of a
// real drag goes through setWindow, whose durationChanged emit reaches
// twTrackMix::updateClip and stales the clip's extent — so resize-clip (and a
// released drag-clip-edge) can never expose a missing invalidation in the slip
// setters themselves. This verb leaves the model in the state the drag holds
// between press and release, which is where W9 lived: the next freeze folds the
// NEW slip in while every already-frozen page keeps the OLD material at the
// same timeline position.
//
// Clip addressing is the resize-clip / set-render-gate one (a path of child
// indices, e.g. "0,0" = first clip of the first top-level lane). Attributes are
// applied in the order startOffset, srcStart, loopStart; each is optional, and
// at least one must be present. Positions are timeline frames; startOffset and
// loopStart are WARPED-domain, srcStart is a SOURCE-domain exact rational.
//
// Transient/test-support: not undoable (no inverse), like drag-clip-edge.
class SSlipClipAction : public SAction {
public:
    SSlipClipAction() = default;

    QString name() const override { return QStringLiteral("slip-clip"); }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

private:
    QList<int> clipPath_;
    bool     hasStartOffset_ = false;
    offset_t startOffset_    = 0;
    bool     hasSrcStart_    = false;
    Fraction srcStart_       = Fraction(0);
    bool     hasLoopStart_   = false;
    offset_t loopStart_      = 0;
};

#endif // SSLIPCLIPACTION_H
