#ifndef SASSERTAUDIOLENGTHACTION_H
#define SASSERTAUDIOLENGTHACTION_H

#include "app/actions/saction.h"

/**
 * Assertion action: verify HOW LONG a rendered audio file is.
 *
 * The other assert-audio-* verbs measure what is inside a window and say
 * nothing about the file's extent — with the render length hardcoded at 60 s
 * they never needed to. Now that a render ends where the arrangement ends, the
 * length IS the behaviour under test, and it needs a verb of its own.
 *
 * XML format:
 * <assert-audio-length filename="render.wav"
 *                      minFrames="0" maxFrames="-1"/>
 *
 * Parameters:
 * - filename:  file in the test output dir (no path separators)
 * - minFrames: lower bound, inclusive (default 0 = no lower bound)
 * - maxFrames: upper bound, inclusive (default -1 = no upper bound)
 *
 * A zero-frame file is a legal answer (an empty arrangement renders one), so
 * this reads the header rather than the samples: an RMS analyser rejects an
 * empty region, which would report the interesting case as an error.
 */
class SAssertAudioLengthAction : public SAction {
public:
    SAssertAudioLengthAction() = default;
    explicit SAssertAudioLengthAction(const QString &filename,
                                      int64_t minFrames = 0,
                                      int64_t maxFrames = -1);

    QString name() const override { return QStringLiteral("assert-audio-length"); }
    QStringList knownAttributes() const override {
        return {QStringLiteral("filename"), QStringLiteral("minFrames"),
                QStringLiteral("maxFrames")};
    }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

private:
    QString filename_;
    int64_t minFrames_ = 0;
    int64_t maxFrames_ = -1;
};

#endif
