#ifndef SASSERTFILEIDENTICALACTION_H
#define SASSERTFILEIDENTICALACTION_H

#include "app/actions/saction.h"

/**
 * Assertion action: two files are byte-identical (optionally over a WAV frame
 * range only).
 *
 * The suite's strongest determinism gate has always been `cmp` of two rendered
 * WAVs, run by hand from a shell — which means no committed case can carry it,
 * and every "the goldens did not move" claim in a PR body was a human's word.
 * This verb puts that compare inside a .qxa: render twice, or render and
 * compare against a file another process produced, and the case fails if a
 * single byte moved.
 *
 * XML format:
 *   <assert-file-identical a="render.wav" b="render2.wav"/>
 *   <assert-file-identical a="render.wav" b="/abs/path/golden.wav"
 *                          startFrame="48000" frameCount="96000"/>
 *
 * Parameters:
 * - a, b:        the two files. PATH RULES, deliberately more permissive than
 *                `render`'s output name: an ABSOLUTE path is used as given
 *                (this is the point — a golden produced by another process,
 *                or kept outside the repo, has to be nameable); a path that
 *                contains a separator is relative to the PROCESS working
 *                directory (ctest runs cases from tests/cases/); a bare name
 *                is resolved in the test output directory, where renders land.
 * - startFrame,  a WAV SAMPLE-DATA range, in frames. Absent (frameCount = -1)
 *   frameCount:  compares the whole files byte for byte, headers included.
 *                Given, both files are parsed as RIFF/WAVE, their formats must
 *                agree, and only the requested frames of the `data` chunk are
 *                compared — so a case can assert "the second half is
 *                identical" without asserting anything about the first.
 *
 * Not undoable (an assertion never is).
 */
class SAssertFileIdenticalAction : public SAction {
public:
    SAssertFileIdenticalAction() = default;
    SAssertFileIdenticalAction( const QString &a, const QString &b,
                                int64_t startFrame = 0,
                                int64_t frameCount = -1 );

    QString name() const override
    { return QStringLiteral("assert-file-identical"); }
    QStringList knownAttributes() const override {
        return {QStringLiteral("a"), QStringLiteral("b"),
                QStringLiteral("startFrame"), QStringLiteral("frameCount")};
    }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString a_;
    QString b_;
    int64_t startFrame_ = 0;
    int64_t frameCount_ = -1;
};

#endif
