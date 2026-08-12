#ifndef SASSERTSOURCEPOSITIONACTION_H
#define SASSERTSOURCEPOSITIONACTION_H

#include "app/actions/saction.h"

/**
 * Assertion action: decode WHICH PART OF THE SOURCE a window of rendered audio
 * came from, and check it against what was expected.
 *
 * The position gate. Every other audio assertion this suite owns — RMS, peak,
 * f0 — is position-BLIND: a render that plays the right material from the wrong
 * place passes all of them, which is why wrong-position playback bugs have
 * historically been found by ear. Fed audio that originates from the
 * position-coded fixture (tests/test_position.wav, written and provable by
 * gen_position_fixture), this verb reports the SOURCE frame the audio carries,
 * so "the clip played, but a block late" fails with both numbers in the message.
 *
 * See tw/core/position_code.h for the encoding. In short: source frames are cut
 * into 4096-frame blocks, each a pure tone on its own DFT bin, and a 4096-frame
 * window that lies inside one block decodes to that block exactly, at any phase
 * offset. A window STRADDLING a block boundary lights two bins and is reported
 * as low confidence — which minConfidence turns into a failure rather than a
 * coin flip between two answers.
 *
 * XML format:
 * <assert-source-position filename="render.wav"
 *                         startFrame="0" frameCount="4096" channel="-1"
 *                         expectSourceFrame="0" tolerance="4096"
 *                         expectSourceFrameAny="0,4096"
 *                         expectSilence="false" minConfidence="3.0"/>
 *
 * Parameters:
 * - filename: WAV file, relative to the test output dir
 * - startFrame: where in THAT file to look
 * - frameCount: window length; 4096 (the block size) is what the encoding is
 *   designed for and the default
 * - channel: channel to decode (-1 = all channels mixed)
 * - expectSourceFrame: the source frame the window should carry, +/- tolerance
 * - tolerance: default 4096 — one block, i.e. "the right block"
 * - expectSourceFrameAny: comma-separated list of ACCEPTABLE source frames.
 *   This is how proposal 16 is expressed as an assertion: while an edit is
 *   absorbed the RT path deliberately serves stale-but-consistent audio, so
 *   BOTH the pre-edit and post-edit source frames are legitimate at that
 *   moment — but any THIRD position is a real bug, and this still fails on it.
 *   An empty list means "not checked"; a non-empty list is checked INSTEAD of
 *   expectSourceFrame.
 * - expectSilence: require the window to be below the silence floor. What
 *   "nothing plays after the clip ends" looks like as an assertion.
 * - minConfidence: minimum best-bin/runner-up ratio (default 3.0). On clean
 *   in-block audio this runs to five or six figures, so the default only ever
 *   rejects genuinely ambiguous windows.
 */
class SAssertSourcePositionAction : public SAction {
public:
    SAssertSourcePositionAction() = default;

    QString name() const override { return QStringLiteral("assert-source-position"); }
    QStringList knownAttributes() const override;
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

private:
    QString filename_;
    int64_t startFrame_ = 0;
    int64_t frameCount_ = 4096;
    int     channel_ = -1;
    int64_t expectSourceFrame_ = -1;      // -1 = not checked
    int64_t tolerance_ = 4096;
    QString expectSourceFrameAny_;        // empty = not checked
    bool    expectSilence_ = false;
    double  minConfidence_ = 3.0;
};

#endif
