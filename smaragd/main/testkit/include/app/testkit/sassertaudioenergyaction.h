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
 * - frameCount: Number of frames to analyze (-1 = to the end of the file)
 * - channel: Channel to analyze (-1 = all channels pooled, 0+ = that channel)
 *
 * `channel` applies WITH OR WITHOUT `frameCount`. It used to be dropped
 * whenever frameCount was omitted, because that took a separate whole-file code
 * path which hard-coded the pooled figure — invisible while every channel of
 * every render is equal, and a silent mis-pass the day the sink goes wide
 * (proposal 36 M0). A channel the file does not have is REJECTED rather than
 * measured as an empty selection, which would report RMS 0 and read like a
 * silent render. `filename` resolves via resolveTestFilePath, so a committed
 * fixture is addressable as well as a render.
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
    QStringList knownAttributes() const override {
        return {QStringLiteral("filename"), QStringLiteral("minRms"),
                QStringLiteral("maxRms"), QStringLiteral("startFrame"),
                QStringLiteral("frameCount"), QStringLiteral("channel")};
    }
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
