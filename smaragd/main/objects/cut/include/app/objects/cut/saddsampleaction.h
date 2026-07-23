#ifndef SADDSAMPLEACTION_H
#define SADDSAMPLEACTION_H

#include "app/actions/saction.h"
#include "tw/graph/tw303aenv.h"
#include "tw/core/twfraction.h"
#include "tw/sources/twgrainparams.h"

class QString;

// Action: add a sample to a track at a given time position.
// Inverse: removes the sample (SRemoveSampleAction).
//
// Two forms:
//   - the plain one wraps the WHOLE wave in a default cut. That is what the
//     <add-sample/> verb and dropping a file on the timeline do.
//   - the windowed one restores an EXACT clip — slip anchor, trim, loop and
//     grain params — and is what SRemoveSampleAction hands back as its
//     inverse. Without it, undoing the deletion of an edited clip silently
//     returned a full-length unedited one, because apply() built a default cut
//     and nothing else.
class SAddSampleAction : public SAction {
public:
    SAddSampleAction() = default;
    SAddSampleAction(int trackIdx, const QString &filePath, offset_t timePos);
    SAddSampleAction(int trackIdx, const QString &filePath, offset_t timePos,
                     const Fraction &srcStart, length_t cutDuration,
                     length_t loopLength, const twGrainParams &grain);

    QString name() const override { return QStringLiteral("add-sample"); }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

private:
    int trackIndex_ = 0;
    QString filePath_;
    offset_t timePos_ = 0;

    // false = wrap the whole wave. Every window attribute below is optional in
    // the XML, and absent means exactly that — so the <add-sample/> verb and
    // every existing .qxa keep their current behaviour untouched.
    bool hasWindow_ = false;
    Fraction srcStart_ = Fraction(0);
    length_t cutDuration_ = 0;
    length_t loopLength_ = 0;
    twGrainParams grain_;
};

#endif // SADDSAMPLEACTION_H
