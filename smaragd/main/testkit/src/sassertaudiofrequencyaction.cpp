#include "app/testkit/sassertaudiofrequencyaction.h"
#include "app/actions/sactionregistry.h"
#include "app/shell/sapplication.h"
#include "tw/analysis/audio_analysis.h"
#include <QDomElement>
#include <QDebug>

SAssertAudioFrequencyAction::SAssertAudioFrequencyAction(const QString &filename,
                                                         double minHz, double maxHz,
                                                         int64_t startFrame,
                                                         int64_t frameCount,
                                                         int channel)
    : filename_(filename), minHz_(minHz), maxHz_(maxHz),
      startFrame_(startFrame), frameCount_(frameCount), channel_(channel)
{
}

SApplyResult SAssertAudioFrequencyAction::apply(SProject * /*project*/)
{
    if (filename_.isEmpty()) {
        qWarning() << "SAssertAudioFrequencyAction: no filename given";
        return {false, nullptr};
    }

    SApplication &app = SApplication::app();
    QString outputDir = app.testOutputDir();
    if (outputDir.isEmpty()) {
        qWarning() << "SAssertAudioFrequencyAction: no test output directory configured";
        return {false, nullptr};
    }

    QString fullPath = outputDir + "/" + filename_;

    std::string error;
    double hz = audio::estimateFundamental(fullPath.toStdString(),
                                           startFrame_, frameCount_,
                                           channel_, error);
    if (!error.empty()) {
        qWarning() << "SAssertAudioFrequencyAction: failed to analyze file:"
                   << QString::fromStdString(error);
        return {false, nullptr};
    }

    if (hz < minHz_ || hz > maxHz_) {
        qWarning() << "SAssertAudioFrequencyAction: fundamental out of range"
                   << "expected [" << minHz_ << "," << maxHz_ << "] Hz"
                   << "got" << hz << "Hz";
        return {false, nullptr};
    }

    qDebug() << "SAssertAudioFrequencyAction: fundamental OK" << hz
             << "Hz in range [" << minHz_ << "," << maxHz_ << "]";

    return {true, nullptr};  // Assertions are not undoable
}

void SAssertAudioFrequencyAction::writeXml(QDomElement &elem) const
{
    elem.setAttribute("filename", filename_);
    elem.setAttribute("minHz", QString::number(minHz_, 'f', 3));
    elem.setAttribute("maxHz", QString::number(maxHz_, 'f', 3));
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

bool SAssertAudioFrequencyAction::readXml(const QDomElement &elem, int /*version*/)
{
    // A missing filename is reported by apply(), not here: readXml failing
    // makes the action undeserializable, which breaks the round-trip audit
    // (it feeds every action a DEFAULT instance through write→read→write).
    filename_ = elem.attribute("filename", "");

    bool ok1, ok2, ok3, ok4, ok5;
    minHz_ = elem.attribute("minHz", "0").toDouble(&ok1);
    maxHz_ = elem.attribute("maxHz", "0").toDouble(&ok2);
    startFrame_ = elem.attribute("startFrame", "0").toLongLong(&ok3);
    frameCount_ = elem.attribute("frameCount", "-1").toLongLong(&ok4);
    channel_ = elem.attribute("channel", "-1").toInt(&ok5);

    if (!ok1 || !ok2 || !ok3 || !ok4 || !ok5) {
        qWarning() << "SAssertAudioFrequencyAction::readXml: invalid numeric attributes";
        return false;
    }

    return true;
}

static const bool s_reg_assert_audio_frequency = (
    SActionRegistry::instance().registerType(
        QStringLiteral("assert-audio-frequency"),
        []{ return new SAssertAudioFrequencyAction; }
    ), true
);
