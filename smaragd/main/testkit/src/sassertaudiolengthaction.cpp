#include "app/testkit/sassertaudiolengthaction.h"
#include "app/actions/sactionregistry.h"
#include "app/shell/sapplication.h"
#include "tw/analysis/audio_analysis.h"
#include <QDomElement>
#include <QDebug>

SAssertAudioLengthAction::SAssertAudioLengthAction(const QString &filename,
                                                   int64_t minFrames,
                                                   int64_t maxFrames)
    : filename_(filename), minFrames_(minFrames), maxFrames_(maxFrames)
{
}

SApplyResult SAssertAudioLengthAction::apply(SProject * /*project*/)
{
    if (filename_.isEmpty()) {
        qWarning() << "SAssertAudioLengthAction: no filename given";
        return {false, nullptr};
    }

    SApplication &app = SApplication::app();
    QString outputDir = app.testOutputDir();
    if (outputDir.isEmpty()) {
        qWarning() << "SAssertAudioLengthAction: no test output directory configured";
        return {false, nullptr};
    }

    QString fullPath = outputDir + "/" + filename_;

    audio::AudioFileInfo info;
    std::string error;
    if (!audio::readAudioFileInfo(fullPath.toStdString(), info, error)) {
        qWarning() << "SAssertAudioLengthAction: cannot read" << fullPath
                   << ":" << QString::fromStdString(error);
        return {false, nullptr};
    }

    if (info.frameCount < minFrames_) {
        qWarning() << "SAssertAudioLengthAction:" << filename_ << "is too SHORT:"
                   << info.frameCount << "frames <" << minFrames_
                   << "(rate" << info.sampleRate << ")";
        return {false, nullptr};
    }
    if (maxFrames_ >= 0 && info.frameCount > maxFrames_) {
        qWarning() << "SAssertAudioLengthAction:" << filename_ << "is too LONG:"
                   << info.frameCount << "frames >" << maxFrames_
                   << "(rate" << info.sampleRate << ")";
        return {false, nullptr};
    }

    qDebug() << "SAssertAudioLengthAction:" << filename_ << "length OK —"
             << info.frameCount << "frames at" << info.sampleRate << "Hz";
    return {true, nullptr};  // Assertions are not undoable
}

void SAssertAudioLengthAction::writeXml(QDomElement &elem) const
{
    elem.setAttribute("filename", filename_);
    if (minFrames_ != 0) {
        elem.setAttribute("minFrames", QString::number(minFrames_));
    }
    if (maxFrames_ != -1) {
        elem.setAttribute("maxFrames", QString::number(maxFrames_));
    }
}

bool SAssertAudioLengthAction::readXml(const QDomElement &elem, int /*version*/)
{
    // A missing filename is reported by apply(), not here — see the note in
    // SAssertAudioPeakAction::readXml (the round-trip audit feeds every action
    // its DEFAULT instance).
    filename_ = elem.attribute("filename", "");

    bool ok1, ok2;
    minFrames_ = elem.attribute("minFrames", "0").toLongLong(&ok1);
    maxFrames_ = elem.attribute("maxFrames", "-1").toLongLong(&ok2);

    if (!ok1 || !ok2) {
        qWarning() << "SAssertAudioLengthAction::readXml: invalid numeric attributes";
        return false;
    }
    return true;
}

static const bool s_reg_assert_audio_length = (
    SActionRegistry::instance().registerType(
        QStringLiteral("assert-audio-length"),
        []{ return new SAssertAudioLengthAction; }
    ), true
);
