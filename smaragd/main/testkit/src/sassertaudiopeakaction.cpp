#include "app/testkit/sassertaudiopeakaction.h"
#include "app/actions/sactionregistry.h"
#include "app/shell/sapplication.h"
#include "tw/analysis/audio_analysis.h"
#include <QDomElement>
#include <QDebug>

SAssertAudioPeakAction::SAssertAudioPeakAction(const QString &filename, double maxPeak,
                                               int64_t startFrame,
                                               int64_t frameCount,
                                               int channel)
    : filename_(filename), maxPeak_(maxPeak),
      startFrame_(startFrame), frameCount_(frameCount), channel_(channel)
{
}

SApplyResult SAssertAudioPeakAction::apply(SProject * /*project*/)
{
    // Construct full path to the audio file
    SApplication &app = SApplication::app();
    QString outputDir = app.testOutputDir();
    if (outputDir.isEmpty()) {
        qWarning() << "SAssertAudioPeakAction: no test output directory configured";
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
        qWarning() << "SAssertAudioPeakAction: failed to analyze file:" << QString::fromStdString(error);
        return {false, nullptr};
    }

    // Check if peak is in range
    bool peakOk = audio::isPeakInRange(metrics, maxPeak_);

    if (!peakOk) {
        qWarning() << "SAssertAudioPeakAction: peak amplitude out of range"
                   << "expected <=" << maxPeak_
                   << "got" << metrics.peakAmplitude;
        return {false, nullptr};
    }

    qDebug() << "SAssertAudioPeakAction: peak amplitude OK"
             << metrics.peakAmplitude << "<=" << maxPeak_;

    return {true, nullptr};  // Assertions are not undoable
}

void SAssertAudioPeakAction::writeXml(QDomElement &elem) const
{
    elem.setAttribute("filename", filename_);
    elem.setAttribute("maxPeak", QString::number(maxPeak_, 'f', 6));
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

bool SAssertAudioPeakAction::readXml(const QDomElement &elem, int /*version*/)
{
    filename_ = elem.attribute("filename", "");
    if (filename_.isEmpty()) {
        qWarning() << "SAssertAudioPeakAction::readXml: missing filename";
        return false;
    }

    bool ok1, ok2, ok3, ok4;
    maxPeak_ = elem.attribute("maxPeak", "0.95").toDouble(&ok1);
    startFrame_ = elem.attribute("startFrame", "0").toLongLong(&ok2);
    frameCount_ = elem.attribute("frameCount", "-1").toLongLong(&ok3);
    channel_ = elem.attribute("channel", "-1").toInt(&ok4);

    if (!ok1 || !ok2 || !ok3 || !ok4) {
        qWarning() << "SAssertAudioPeakAction::readXml: invalid numeric attributes";
        return false;
    }

    return true;
}

static const bool s_reg_assert_audio_peak = (
    SActionRegistry::instance().registerType(
        QStringLiteral("assert-audio-peak"),
        []{ return new SAssertAudioPeakAction; }
    ), true
);
