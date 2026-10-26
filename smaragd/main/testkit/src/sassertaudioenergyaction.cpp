#include "app/testkit/sassertaudioenergyaction.h"
#include "app/testkit/stestfilepath.h"
#include "app/actions/sactionregistry.h"
#include "app/shell/sapplication.h"
#include "tw/analysis/audio_analysis.h"
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

SApplyResult SAssertAudioEnergyAction::apply(SProject *project)
{
    if (filename_.isEmpty()) {
        qWarning() << "SAssertAudioEnergyAction: no filename given";
        return {false, nullptr};
    }

    // Construct full path to the audio file
    SApplication &app = SApplication::app();
    QString outputDir = app.testOutputDir();
    if (outputDir.isEmpty()) {
        qWarning() << "SAssertAudioEnergyAction: no test output directory configured";
        return {false, nullptr};
    }

    QString fullPath = resolveTestFilePath(filename_, outputDir, project);

    // THE WINDOW, ANCHORED OR ABSOLUTE (QBX-228).
    //
    // With anchorOnset, `startFrame` is an offset FROM where energy begins
    // rather than a position in the file, because captured frame 0 is the
    // DEVICE START and the leading silence in front of it is a wall-clock
    // quantity of the machine. The resolved absolute window is LOGGED either
    // way on a failure: "out of range" without saying where it measured is
    // what made master_closure_linear_ring cost two sessions.
    int64_t windowStart = startFrame_;
    int64_t onsetFrame  = -1;
    if (anchorOnset_) {
        std::string onsetErr;
        if (!audio::findOnsetFrame(fullPath.toStdString(), 0, channel_,
                                   onsetThreshold_, onsetWindow_,
                                   onsetFrame, onsetErr)) {
            qWarning() << "SAssertAudioEnergyAction: anchorOnset: cannot read"
                       << fullPath << ":" << QString::fromStdString(onsetErr);
            return {false, nullptr};
        }
        if (onsetFrame < 0) {
            qWarning() << "SAssertAudioEnergyAction: anchorOnset:" << filename_
                       << "never reaches RMS" << onsetThreshold_
                       << "- there is no onset to anchor on";
            return {false, nullptr};
        }
        windowStart = onsetFrame + startFrame_;
    }

    // Analyze the audio file. ONE call, region or not: the whole-file path used
    // to be a separate call that hard-coded channel = -1, so `channel=` was
    // silently ignored whenever frameCount was omitted. A
    // negative frameCount now means "to the end of the file" inside the
    // analyzer, which is the only difference between the two cases.
    std::string error;
    audio::AcousticMetrics metrics =
        audio::analyzeWavFileRegion(fullPath.toStdString(),
                                    windowStart, frameCount_,
                                    channel_, error);

    if (!error.empty()) {
        qWarning() << "SAssertAudioEnergyAction: failed to analyze file:"
                   << fullPath << ":" << QString::fromStdString(error);
        return {false, nullptr};
    }

    // Check if energy is in range
    bool energyOk = audio::isEnergyInRange(metrics, minRms_, maxRms_);

    if (!energyOk) {
        qWarning() << "SAssertAudioEnergyAction:" << filename_
                   << "channel" << channel_ << "(-1 = all pooled)"
                   << "RMS energy out of range"
                   << "expected [" << minRms_ << ", " << maxRms_ << "]"
                   << "got" << metrics.rmsEnergy
                   // WHERE it measured, so a red CI run is diagnosable from
                   // its own log. With an anchor the onset is the quantity
                   // that differs between machines, so it is named too.
                   << "over [" << windowStart << ","
                   << ( frameCount_ < 0 ? -1 : windowStart + frameCount_ ) << ")"
                   << ( anchorOnset_
                            ? QStringLiteral( "anchored on onset %1 + %2" )
                                  .arg( onsetFrame ).arg( startFrame_ )
                            : QStringLiteral( "absolute" ) );
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
    if (anchorOnset_) {
        elem.setAttribute("anchorOnset", "1");
        elem.setAttribute("onsetThreshold", QString::number(onsetThreshold_, 'f', 6));
        elem.setAttribute("onsetWindow", QString::number(onsetWindow_));
    }
}

bool SAssertAudioEnergyAction::readXml(const QDomElement &elem, int /*version*/)
{
    // A missing filename is reported by apply(), not here: readXml failing
    // makes the action undeserializable, which breaks the round-trip audit
    // (it feeds every action a DEFAULT instance through write→read→write).
    filename_ = elem.attribute("filename", "");

    bool ok1, ok2, ok3, ok4, ok5;
    minRms_ = elem.attribute("minRms", "0.01").toDouble(&ok1);
    maxRms_ = elem.attribute("maxRms", "0.95").toDouble(&ok2);
    startFrame_ = elem.attribute("startFrame", "0").toLongLong(&ok3);
    frameCount_ = elem.attribute("frameCount", "-1").toLongLong(&ok4);
    channel_ = elem.attribute("channel", "-1").toInt(&ok5);

    bool ok6, ok7;
    anchorOnset_    = elem.attribute("anchorOnset", "0").toInt() != 0;
    onsetThreshold_ = elem.attribute("onsetThreshold", "0.02").toDouble(&ok6);
    onsetWindow_    = elem.attribute("onsetWindow", "64").toLongLong(&ok7);
    if (!ok6 || !ok7) {
        qWarning() << "SAssertAudioEnergyAction::readXml: invalid onset attributes";
        return false;
    }

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
