#include "app/testkit/sassertchannelsdifferaction.h"
#include "app/testkit/stestfilepath.h"
#include "app/actions/sactionregistry.h"
#include "app/shell/sapplication.h"
#include "tw/analysis/audio_analysis.h"
#include <QDomElement>
#include <QDebug>
#include <cmath>

SAssertChannelsDifferAction::SAssertChannelsDifferAction(const QString &filename,
                                                         int channelA, int channelB,
                                                         double minRmsDelta,
                                                         double minDiffRms,
                                                         int64_t startFrame,
                                                         int64_t frameCount)
    : filename_(filename), channelA_(channelA), channelB_(channelB),
      minRmsDelta_(minRmsDelta), minDiffRms_(minDiffRms),
      startFrame_(startFrame), frameCount_(frameCount)
{
}

SApplyResult SAssertChannelsDifferAction::apply(SProject *project)
{
    if (filename_.isEmpty()) {
        qWarning() << "SAssertChannelsDifferAction: no filename given";
        return {false, nullptr};
    }

    SApplication &app = SApplication::app();
    const QString outputDir = app.testOutputDir();
    if (outputDir.isEmpty()) {
        qWarning() << "SAssertChannelsDifferAction: no test output directory configured";
        return {false, nullptr};
    }

    const QString fullPath = resolveTestFilePath(filename_, outputDir, project);

    double rmsA = 0.0, rmsB = 0.0, rmsDiff = 0.0;
    std::string error;
    if (!audio::compareWavChannels(fullPath.toStdString(),
                                   startFrame_, frameCount_,
                                   channelA_, channelB_,
                                   rmsA, rmsB, rmsDiff, error)) {
        qWarning() << "SAssertChannelsDifferAction: failed to analyze file:"
                   << fullPath << ":" << QString::fromStdString(error);
        return {false, nullptr};
    }

    const double delta = std::abs(rmsA - rmsB);

    // Every message carries BOTH numbers. "The levels match" and "the samples
    // match" are different findings with different causes, and the one that was
    // not asserted on is usually what explains the one that was.
    const QString where =
        QString("%1 ch%2 vs ch%3 @ %4 (%5 frames): rms %6 vs %7, delta %8, rms(A-B) %9")
            .arg(filename_).arg(channelA_).arg(channelB_)
            .arg(startFrame_).arg(frameCount_)
            .arg(rmsA, 0, 'f', 6).arg(rmsB, 0, 'f', 6)
            .arg(delta, 0, 'f', 6).arg(rmsDiff, 0, 'f', 6);

    if (delta < minRmsDelta_) {
        qWarning() << "SAssertChannelsDifferAction:" << where
                   << "- level delta below minRmsDelta" << minRmsDelta_
                   << "(the channels carry the same LEVEL; a sink that "
                      "duplicates one bus looks exactly like this)";
        return {false, nullptr};
    }

    if (minDiffRms_ >= 0.0 && rmsDiff < minDiffRms_) {
        qWarning() << "SAssertChannelsDifferAction:" << where
                   << "- difference-signal RMS below minDiffRms" << minDiffRms_
                   << "(the channels hold the same audio sample for sample)";
        return {false, nullptr};
    }

    qDebug() << "SAssertChannelsDifferAction:" << where << "OK";
    return {true, nullptr};  // Assertions are not undoable
}

void SAssertChannelsDifferAction::writeXml(QDomElement &elem) const
{
    elem.setAttribute("filename", filename_);
    elem.setAttribute("channelA", QString::number(channelA_));
    elem.setAttribute("channelB", QString::number(channelB_));
    elem.setAttribute("minRmsDelta", QString::number(minRmsDelta_, 'f', 6));
    if (minDiffRms_ >= 0.0) {
        elem.setAttribute("minDiffRms", QString::number(minDiffRms_, 'f', 6));
    }
    if (startFrame_ != 0) {
        elem.setAttribute("startFrame", QString::number(startFrame_));
    }
    if (frameCount_ != -1) {
        elem.setAttribute("frameCount", QString::number(frameCount_));
    }
}

bool SAssertChannelsDifferAction::readXml(const QDomElement &elem, int /*version*/)
{
    // A missing filename is reported by apply(), not here: readXml failing
    // makes the action undeserializable, which breaks the round-trip audit
    // (it feeds every action a DEFAULT instance through write→read→write).
    filename_ = elem.attribute("filename", "");

    bool ok1, ok2, ok3, ok4, ok5, ok6;
    channelA_    = elem.attribute("channelA", "0").toInt(&ok1);
    channelB_    = elem.attribute("channelB", "1").toInt(&ok2);
    minRmsDelta_ = elem.attribute("minRmsDelta", "0.01").toDouble(&ok3);
    minDiffRms_  = elem.attribute("minDiffRms", "-1").toDouble(&ok4);
    startFrame_  = elem.attribute("startFrame", "0").toLongLong(&ok5);
    frameCount_  = elem.attribute("frameCount", "-1").toLongLong(&ok6);

    if (!ok1 || !ok2 || !ok3 || !ok4 || !ok5 || !ok6) {
        qWarning() << "SAssertChannelsDifferAction::readXml: invalid numeric attributes";
        return false;
    }

    return true;
}

static const bool s_reg_assert_channels_differ = (
    SActionRegistry::instance().registerType(
        QStringLiteral("assert-channels-differ"),
        []{ return new SAssertChannelsDifferAction; }
    ), true
);
