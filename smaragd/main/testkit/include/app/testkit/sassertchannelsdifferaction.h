#ifndef SASSERTCHANNELSDIFFERACTION_H
#define SASSERTCHANNELSDIFFERACTION_H

#include "app/actions/saction.h"

/**
 * Assertion action: verify that two channels of a WAV carry DIFFERENT audio.
 *
 * Before this verb, "these channels are genuinely
 * different" could only be INFERRED — by asserting a per-channel RMS
 * band on each and reading the two results against each other, which needs the
 * expected numbers to be known in advance and says nothing when they happen to
 * coincide. Worse, the sink duplicates one bus into every channel today
 * (plugins/CONTRACT.md, "never write an L != R assertion"), so any case that
 * appeared to prove a difference was proving nothing.
 *
 * Two measurements, one pass:
 *
 *  - `minRmsDelta` — |rms(A) - rms(B)| must be at least this. A LEVEL
 *    discriminator: it is what a duplicated bus fails, and it is robust to
 *    everything a level meter is robust to.
 *  - `minDiffRms` — rms(A - B) must be at least this. A CONTENT discriminator,
 *    off by default (-1). Two channels can hold the same level and different
 *    audio (opposite polarity, a different partial, a different clip); the
 *    level test calls those identical and this one does not.
 *
 * Both numbers are logged whichever is asserted on, because the interesting
 * failure is usually the pair.
 *
 * XML format:
 * <assert-channels-differ filename="render.wav" channelA="0" channelB="1"
 *                          minRmsDelta="0.01" minDiffRms="-1"
 *                          startFrame="0" frameCount="-1"/>
 *
 * Parameters:
 * - filename: WAV to read (test output dir, else the .qxa's directory)
 * - channelA / channelB: the two channel indices; must differ and must exist
 * - minRmsDelta: minimum |rms(A) - rms(B)| (default 0.01)
 * - minDiffRms: minimum rms(A - B); < 0 = not checked (default -1)
 * - startFrame: frame to start at (default 0)
 * - frameCount: frames to analyse (-1 = to the end of the file)
 *
 * Pair it with `expectReject="true"` to assert the OPPOSITE — that two channels
 * are the same audio. That is the shape of today's duplicated-mono sink, and
 * the day the sink goes wide such a case is supposed to fail — that is the
 * signal it exists to give, not a regression to loosen.
 */
class SAssertChannelsDifferAction : public SAction {
public:
    SAssertChannelsDifferAction() = default;
    explicit SAssertChannelsDifferAction(const QString &filename,
                                         int channelA, int channelB,
                                         double minRmsDelta = 0.01,
                                         double minDiffRms = -1.0,
                                         int64_t startFrame = 0,
                                         int64_t frameCount = -1);

    QString name() const override { return QStringLiteral("assert-channels-differ"); }
    QStringList knownAttributes() const override {
        return {QStringLiteral("filename"), QStringLiteral("channelA"),
                QStringLiteral("channelB"), QStringLiteral("minRmsDelta"),
                QStringLiteral("minDiffRms"), QStringLiteral("startFrame"),
                QStringLiteral("frameCount")};
    }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

private:
    QString filename_;
    int channelA_ = 0;
    int channelB_ = 1;
    double minRmsDelta_ = 0.01;
    double minDiffRms_ = -1.0;
    int64_t startFrame_ = 0;
    int64_t frameCount_ = -1;
};

#endif // SASSERTCHANNELSDIFFERACTION_H
