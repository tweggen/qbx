#include "actions/sassertaudioenergyaction.h"
#include "sactionregistry.h"
#include "sapplication.h"
#include "audio_analysis.h"
#include <QDomElement>
#include <QDebug>

SAssertAudioEnergyAction::SAssertAudioEnergyAction(const QString &filename,
                                                   double minRms, double maxRms,
                                                   int64_t startFrame,
                                                   int64_t frameCount,
                                                   int channel)
    : filename_(filename), minRms_(minRms), maxRms_(maxRms),
      startFrame_(startFrame), frameCount_(frameCount), channel_(channel)
{
}

SApplyResult SAssertAudioEnergyAction::apply(SProject * /*project*/)
{
    // Construct full path to the audio file
    SApplication &app = SApplication::app();
    QString outputDir = app.testOutputDir();
    if (outputDir.isEmpty()) {
        qWarning() << "SAssertAudioEnergyAction: no test output directory configured";
        return {false, nullptr};
    }

    QString fullPath = outputDir + "/" + filename_;

    // Analyze the audio file
    std::string error;
    audio::AcousticMetrics metrics;

    if (frameCount_ == -1) {
        // Analyze entire file
        metrics = audio::analyzeWavFile(fullPath.toStdString(), error);
    } else {
        // Analyze specific region
        metrics = audio::analyzeWavFileRegion(fullPath.toStdString(),
                                             startFrame_, frameCount_,
                                             channel_, error);
    }

    if (!error.empty()) {
        qWarning() << "SAssertAudioEnergyAction: failed to analyze file:" << QString::fromStdString(error);
        return {false, nullptr};
    }

    // Check if energy is in range
    bool energyOk = audio::isEnergyInRange(metrics, minRms_, maxRms_);

    if (!energyOk) {
        qWarning() << "SAssertAudioEnergyAction: RMS energy out of range"
                   << "expected [" << minRms_ << ", " << maxRms_ << "]"
                   << "got" << metrics.rmsEnergy;
        return {false, nullptr};
    }

    qDebug() << "SAssertAudioEnergyAction: RMS energy OK"
             << metrics.rmsEnergy << "in range [" << minRms_ << ", " << maxRms_ << "]";

    return {true, nullptr};  // Assertions are not undoable
}

void SAssertAudioEnergyAction::writeXml(QDomElement &elem) const
{
    elem.setAttribute("filename", filename_);
    elem.setAttribute("minRms", QString::number(minRms_, 'f', 6));
    elem.setAttribute("maxRms", QString::number(maxRms_, 'f', 6));
    if (startFrame_ != 0) {
        elem.setAttribute("startFrame", QString::number(startFrame_));
    }
    if (frameCount_ != -1) {
        elem.setAttribute("frameCount", QString::number(frameCount_));
    }
    if (channel_ != -1) {
        elem.setAttribute("channel", QString::number(channel_));
    }
}

bool SAssertAudioEnergyAction::readXml(const QDomElement &elem, int /*version*/)
{
    filename_ = elem.attribute("filename", "");
    if (filename_.isEmpty()) {
        qWarning() << "SAssertAudioEnergyAction::readXml: missing filename";
        return false;
    }

    bool ok1, ok2, ok3, ok4, ok5;
    minRms_ = elem.attribute("minRms", "0.01").toDouble(&ok1);
    maxRms_ = elem.attribute("maxRms", "0.95").toDouble(&ok2);
    startFrame_ = elem.attribute("startFrame", "0").toLongLong(&ok3);
    frameCount_ = elem.attribute("frameCount", "-1").toLongLong(&ok4);
    channel_ = elem.attribute("channel", "-1").toInt(&ok5);

    if (!ok1 || !ok2 || !ok3 || !ok4 || !ok5) {
        qWarning() << "SAssertAudioEnergyAction::readXml: invalid numeric attributes";
        return false;
    }

    return true;
}

static const bool s_reg_assert_audio_energy = (
    SActionRegistry::instance().registerType(
        QStringLiteral("assert-audio-energy"),
        []{ return new SAssertAudioEnergyAction; }
    ), true
);
