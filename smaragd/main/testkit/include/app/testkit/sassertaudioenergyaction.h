#ifndef SASSERTAUDIOENERGYACTION_H
#define SASSERTAUDIOENERGYACTION_H

#include "app/actions/saction.h"

/**
 * Assertion action: Verify acoustic energy (RMS) of a rendered audio file.
 *
 * Checks that the RMS energy of a specific region or entire file falls
 * within expected bounds. Useful for verifying:
 * - Audio is not silent (minRms > 0)
 * - Audio is not clipped (maxRms < 1.0)
 * - Grain processing was applied (energy distribution changed)
 * - Volume adjustments took effect
 *
 * XML format:
 * <assert-audio-energy filename="render.wav"
 *                       minRms="0.01" maxRms="0.95"
 *                       startFrame="0" frameCount="-1"
 *                       channel="-1"/>
 *
 * Parameters:
 * - filename: Path to WAV file (relative to test output dir)
 * - minRms: Minimum acceptable RMS energy (0.0 to 1.0)
 * - maxRms: Maximum acceptable RMS energy (0.0 to 1.0)
 * - startFrame: Frame to start analysis (default: 0)
 * - frameCount: Number of frames to analyze (-1 = entire file)
 * - channel: Channel to analyze (-1 = all channels mixed, 0+ = specific channel)
 */
class SAssertAudioEnergyAction : public SAction {
public:
    SAssertAudioEnergyAction() = default;
    explicit SAssertAudioEnergyAction(const QString &filename,
                                      double minRms, double maxRms,
                                      int64_t startFrame = 0,
                                      int64_t frameCount = -1,
                                      int channel = -1);

    QString name() const override { return QStringLiteral("assert-audio-energy"); }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

private:
    QString filename_;
    double minRms_ = 0.01;
    double maxRms_ = 0.95;
    int64_t startFrame_ = 0;
    int64_t frameCount_ = -1;
    int channel_ = -1;
};

#endif
