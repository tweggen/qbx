#ifndef SASSERTFILEIDENTICALACTION_H
#define SASSERTFILEIDENTICALACTION_H

#include "app/actions/saction.h"

/**
 * Assertion action: two files are byte-for-byte identical.
 *
 * This repo has gated render exactness by `cmp` since the beginning, but only
 * ever BETWEEN RUNS — two renders produced by the same session, or by two
 * builds, compared by hand outside the harness. There was no verb, so
 * "byte-identical to a committed golden" could be stated in a PR body and could
 * not be enforced by the suite. This verb makes it enforceable across machines
 * and weeks, not just within one session.
 *
 * Both paths go through resolveTestFilePath, so `actual` is
 * normally a render in the test output directory, and `expected` is either a
 * second render beside it or a committed reference addressed relative to the
 * .qxa — e.g. `expected="../test_sawtooth.wav"`.
 *
 * ON MISMATCH IT DIAGNOSES. "not identical" is not a finding: the message
 * carries both sizes, the offset of the FIRST differing byte, the two byte
 * values there, and how many bytes differ in the overlapping region. A single
 * flipped sample and a wholesale re-render look nothing alike in that message,
 * and telling them apart is the entire reason to have the verb rather than a
 * hash.
 *
 * XML format:
 * <assert-file-identical actual="render_a.wav" expected="render_b.wav"/>
 *
 * Parameters:
 * - actual:   the file under test (test output dir, else the .qxa's directory,
 *             else the cwd; an ABSOLUTE path is used as given - a golden
 *             produced by ANOTHER PROCESS has to be nameable)
 * - expected: the reference file, resolved the same way
 * - maxReportedDiffs: how many differing offsets to list individually
 *                     (default 8; 0 = just the summary)
 * - startFrame / frameCount (proposal 37 P0a): given a range (frameCount >= 0
 *             or startFrame != 0), both files are parsed as RIFF/WAVE, their
 *             formats must agree, and only those frames of the data chunk are
 *             compared - so a case can assert "identical BEFORE the edit" while
 *             the tail legitimately differs. Default = whole files, headers
 *             included, exactly what cmp does.
 *
 * Pair it with `expectReject="true"` to assert that two files DIFFER — which is
 * how a case proves the gate can fail (a gate never seen to fail is not known to
 * be a gate).
 */
class SAssertFileIdenticalAction : public SAction {
public:
    SAssertFileIdenticalAction() = default;
    SAssertFileIdenticalAction( const QString &actual, const QString &expected,
                                int maxReportedDiffs = 8,
                                qint64 startFrame = 0, qint64 frameCount = -1 );

    QString name() const override { return QStringLiteral("assert-file-identical"); }
    QStringList knownAttributes() const override {
        return {QStringLiteral("actual"), QStringLiteral("expected"),
                QStringLiteral("maxReportedDiffs"),
                QStringLiteral("startFrame"), QStringLiteral("frameCount")};
    }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

private:
    QString actual_;
    QString expected_;
    int     maxReportedDiffs_ = 8;
    qint64  startFrame_ = 0;
    qint64  frameCount_ = -1;

    SApplyResult compareWavRange_( const QByteArray &a, const QByteArray &e );
};

#endif // SASSERTFILEIDENTICALACTION_H
