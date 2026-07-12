#ifndef SASSERTAUDIOPEAKACTION_H
#define SASSERTAUDIOPEAKACTION_H

#include "app/actions/saction.h"

/**
 * Assertion action: Verify peak amplitude of a rendered audio file.
 *
 * Checks that the maximum absolute amplitude doesn't exceed a threshold.
 * Useful for verifying:
 * - Audio is not clipped (peak < 1.0)
 * - Audio has reasonable headroom (peak < 0.9)
 * - Silence detection (peak < 0.001)
 *
 * XML format:
 * <assert-audio-peak filename="render.wav"
 *                     maxPeak="0.95"
 *                     startFrame="0" frameCount="-1"
 *                     channel="-1"/>
 *
 * Parameters:
 * - filename: Path to WAV file (relative to test output dir)
 * - maxPeak: Maximum acceptable peak amplitude (0.0 to 1.0)
 * - startFrame: Frame to start analysis (default: 0)
 * - frameCount: Number of frames to analyze (-1 = entire file)
 * - channel: Channel to analyze (-1 = all channels, 0+ = specific channel)
 */
class SAssertAudioPeakAction : public SAction {
public:
    SAssertAudioPeakAction() = default;
    explicit SAssertAudioPeakAction(const QString &filename, double maxPeak,
                                    int64_t startFrame = 0,
                                    int64_t frameCount = -1,
                                    int channel = -1);

    QString name() const override { return QStringLiteral("assert-audio-peak"); }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

private:
    QString filename_;
    double maxPeak_ = 0.95;
    int64_t startFrame_ = 0;
    int64_t frameCount_ = -1;
    int channel_ = -1;
};

#endif
