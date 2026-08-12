#include "app/testkit/sdumpplaybackcaptureaction.h"

#include "app/actions/sactionregistry.h"
#include "app/shell/sapplication.h"

#include "tw/devices/capture_backend.h"
#include "tw/playback/twspeaker.h"
#include "tw/sinks/audio_file_writer.h"

#include <QDomElement>
#include <QDebug>

QStringList SDumpPlaybackCaptureAction::knownAttributes() const
{
    return {
        QStringLiteral("filename"),
        QStringLiteral("minFrames"),
    };
}

SApplyResult SDumpPlaybackCaptureAction::apply(SProject * /*project*/)
{
    if (filename_.isEmpty()) {
        qWarning() << "dump-playback-capture: no filename given";
        return {false, nullptr};
    }
    // Same sanitisation as <screenshot>: the verb writes into the test output
    // directory and nowhere else.
    if (filename_.contains("/") || filename_.contains("\\") || filename_.contains("..")) {
        qWarning() << "dump-playback-capture: filename contains path separators:"
                   << filename_;
        return {false, nullptr};
    }

    SApplication &app = SApplication::app();
    const QString outputDir = app.testOutputDir();
    if (outputDir.isEmpty()) {
        qWarning() << "dump-playback-capture: no test output directory configured";
        return {false, nullptr};
    }
    if (!app.ensureOutputDirExists()) {
        qWarning() << "dump-playback-capture: failed to create output directory:"
                   << outputDir;
        return {false, nullptr};
    }

    std::shared_ptr<twSpeaker> speaker = app.getSpeaker();
    audio::AudioBackend *backend = speaker ? speaker->getBackend() : nullptr;
    if (!backend) {
        qWarning() << "dump-playback-capture: no audio backend";
        return {false, nullptr};
    }

    audio::CaptureBackend *capture = dynamic_cast<audio::CaptureBackend *>(backend);
    if (!capture) {
        qWarning() << "dump-playback-capture: the active audio backend is"
                   << backend->name()
                   << "- this verb needs the capture backend. A headless"
                   << "(--test-case) run selects it by default; otherwise set"
                   << "SMARAGD_AUDIO_BACKEND=capture.";
        return {false, nullptr};
    }

    const audio::CaptureBackend::CaptureBuffer buf = capture->capturedAudio();
    const qint64 frames = (qint64) buf.frames();

    if (buf.droppedFrames > 0) {
        qWarning() << "dump-playback-capture: the recording is TRUNCATED —"
                   << (qulonglong) buf.droppedFrames
                   << "frames were dropped at the capture cap";
        return {false, nullptr};
    }
    if (frames <= 0) {
        qWarning() << "dump-playback-capture: nothing was captured. Playback"
                   << "either never started or produced no callbacks.";
        return {false, nullptr};
    }
    if (frames < minFrames_) {
        qWarning() << "dump-playback-capture: captured only" << frames
                   << "frames, expected at least" << minFrames_;
        return {false, nullptr};
    }

    audio::AudioFileConfig cfg;
    cfg.sampleRate = buf.sampleRate;
    cfg.channels   = buf.channels;
    cfg.sampleType = twSampleType::Int16;

    // The SAME writer the render path uses (16-bit PCM WAV), so a captured
    // playback and a render of the same material are comparable files rather
    // than two spellings of one idea.
    std::unique_ptr<audio::AudioFileWriter> writer =
        audio::createAudioFileWriter(audio::AudioFormat::WAV);
    if (!writer) {
        qWarning() << "dump-playback-capture: could not create a WAV writer";
        return {false, nullptr};
    }

    const QString fullPath = outputDir + "/" + filename_;
    if (!writer->open(fullPath.toStdString(), cfg)) {
        qWarning() << "dump-playback-capture: failed to open" << fullPath
                   << ":" << writer->errorMessage();
        return {false, nullptr};
    }
    if (!writer->write(buf.samples.data(), (std::size_t) frames)) {
        qWarning() << "dump-playback-capture: failed to write" << fullPath
                   << ":" << writer->errorMessage();
        writer->close();
        return {false, nullptr};
    }
    if (!writer->close()) {
        qWarning() << "dump-playback-capture: failed to close" << fullPath
                   << ":" << writer->errorMessage();
        return {false, nullptr};
    }

    qDebug() << "dump-playback-capture: wrote" << frames << "frames ("
             << (double) frames / (double) (buf.sampleRate ? buf.sampleRate : 1)
             << "s," << buf.channels << "ch @" << buf.sampleRate << "Hz) to"
             << fullPath;
    return {true, nullptr};  // Not undoable: it produces an artifact.
}

void SDumpPlaybackCaptureAction::writeXml(QDomElement &elem) const
{
    elem.setAttribute("filename", filename_);
    if (minFrames_ > 0) {
        elem.setAttribute("minFrames", QString::number(minFrames_));
    }
}

bool SDumpPlaybackCaptureAction::readXml(const QDomElement &elem, int /*version*/)
{
    // An empty filename is reported by apply(), not here: a readXml that fails
    // makes the verb undeserializable and breaks the round-trip audit.
    filename_  = elem.attribute("filename", "");
    minFrames_ = elem.attribute("minFrames", "0").toLongLong();
    return true;
}

static const bool s_reg_dump_playback_capture = (
    SActionRegistry::instance().registerType(
        QStringLiteral("dump-playback-capture"),
        []{ return new SDumpPlaybackCaptureAction; }
    ), true
);
