#ifndef SASSERTAUDIOFREQUENCYACTION_H
#define SASSERTAUDIOFREQUENCYACTION_H

#include "app/actions/saction.h"

/**
 * Assertion action: verify the fundamental frequency of a rendered region.
 *
 * The pitch gate. RMS and peak assertions cannot distinguish a transposed
 * render from an untransposed one — this one can, so it is what proves the
 * grain transposition (`set-pitch`) actually reached the audio. Detection is
 * autocorrelation (audio::estimateFundamental), so the region must be a
 * strongly periodic signal such as the sawtooth fixture.
 *
 * XML format:
 * <assert-audio-frequency filename="render.wav"
 *                          minHz="854" maxHz="906"
 *                          startFrame="24000" frameCount="24000"
 *                          channel="-1"/>
 *
 * Parameters:
 * - filename: Path to WAV file (relative to test output dir)
 * - minHz/maxHz: accepted band for the detected fundamental
 * - startFrame: Frame to start analysis (default: 0)
 * - frameCount: Number of frames to analyze (-1 = entire file)
 * - channel: Channel to analyze (-1 = all channels mixed, 0+ = specific channel)
 */
class SAssertAudioFrequencyAction : public SAction {
public:
    SAssertAudioFrequencyAction() = default;
    explicit SAssertAudioFrequencyAction(const QString &filename,
                                         double minHz, double maxHz,
                                         int64_t startFrame = 0,
                                         int64_t frameCount = -1,
                                         int channel = -1);

    QString name() const override { return QStringLiteral("assert-audio-frequency"); }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

private:
    QString filename_;
    double minHz_ = 0.0;
    double maxHz_ = 0.0;
    int64_t startFrame_ = 0;
    int64_t frameCount_ = -1;
    int channel_ = -1;
};

#endif
