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
 * not be enforced by the suite. Proposal 36's whole safety argument depends on
 * that being enforceable across milestones, machines and weeks (§5), so B1a adds
 * it before the page grows a channel dimension.
 *
 * Both paths go through resolveTestFilePath (proposal 36 M0), so `actual` is
 * normally a render in the test output directory and `expected` is normally a
 * committed fixture addressed relative to the .qxa — e.g.
 * `expected="../goldens/mc_mono.wav"`.
 *
 * ON MISMATCH IT DIAGNOSES. "not identical" is not a finding: the message
 * carries both sizes, the offset of the FIRST differing byte, the two byte
 * values there, and how many bytes differ in the overlapping region. A single
 * flipped sample and a wholesale re-render look nothing alike in that message,
 * and telling them apart is the entire reason to have the verb rather than a
 * hash.
 *
 * XML format:
 * <assert-file-identical actual="render.wav" expected="../goldens/mc_mono.wav"/>
 *
 * Parameters:
 * - actual:   the file under test (test output dir, else the .qxa's directory,
 *             else the cwd)
 * - expected: the reference file, resolved the same way
 * - maxReportedDiffs: how many differing offsets to list individually
 *                     (default 8; 0 = just the summary)
 *
 * Pair it with `expectReject="true"` to assert that two files DIFFER — which is
 * how the corpus cases prove the gate can fail (a gate never seen to fail is not
 * known to be a gate).
 */
class SAssertFileIdenticalAction : public SAction {
public:
    SAssertFileIdenticalAction() = default;
    SAssertFileIdenticalAction( const QString &actual, const QString &expected,
                                int maxReportedDiffs = 8 );

    QString name() const override { return QStringLiteral("assert-file-identical"); }
    QStringList knownAttributes() const override {
        return {QStringLiteral("actual"), QStringLiteral("expected"),
                QStringLiteral("maxReportedDiffs")};
    }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

private:
    QString actual_;
    QString expected_;
    int     maxReportedDiffs_ = 8;
};

#endif // SASSERTFILEIDENTICALACTION_H
