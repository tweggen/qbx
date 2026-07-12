#include "app/actions/srenderaction.h"
#include "app/model/sappcontext.h"
#include "app/model/sproject.h"
#include "app/actions/sactionregistry.h"
#include "app/model/sobject.h"
#include "tw/render/render_session.h"
#include <QDomElement>
#include <QDebug>
#include <thread>
#include <chrono>

SRenderAction::SRenderAction(const QString &filename, Format format, int quality)
    : filename_(filename), format_(format), quality_(quality)
{
}

SApplyResult SRenderAction::apply(SProject *project)
{
    // Validate preconditions
    if (filename_.isEmpty()) {
        return {false, nullptr};
    }

    // Sanitize filename: reject paths with / or .. to prevent directory traversal
    if (filename_.contains("/") || filename_.contains("\\") || filename_.contains("..")) {
        qWarning() << "SRenderAction: filename contains path separators:" << filename_;
        return {false, nullptr};
    }

    if (!project) {
        qWarning() << "SRenderAction: no project";
        return {false, nullptr};
    }

    SAppContext &app = SAppContext::get();
    QString outputDir = app.testOutputDir();
    if (outputDir.isEmpty()) {
        qWarning() << "SRenderAction: no test output directory configured";
        return {false, nullptr};
    }

    if (!app.ensureOutputDirExists()) {
        qWarning() << "SRenderAction: failed to create output directory:" << outputDir;
        return {false, nullptr};
    }

    // Construct full output path
    QString fullPath = outputDir + "/" + filename_;

    // Determine audio format
    audio::AudioFormat audioFormat;
    switch (format_) {
        case Format::WAV:
            audioFormat = audio::AudioFormat::WAV;
            break;
        case Format::OGG:
            audioFormat = audio::AudioFormat::OGG;
            break;
        case Format::MP3:
            audioFormat = audio::AudioFormat::MP3;
            break;
    }

    // Get synth output component
    twComponent *synthOutput = nullptr;
    SObject *root = project->getRootComponent();
    if (root) {
        synthOutput = &root->getRootComponent();
    }

    if (!synthOutput) {
        qWarning() << "SRenderAction: no synth output component";
        return {false, nullptr};
    }

    // Set up render parameters
    audio::RenderParams params;
    params.outputPath = fullPath.toStdString();
    params.format = audioFormat;
    params.quality = quality_;
    params.extent = audio::RenderParams::Extent::EntireProject;
    params.startTimeSec = 0.0;
    params.endTimeSec = project->getDurationSeconds();

    // Start rendering
    // Note: RenderSession is asynchronous. For test mode, we should wait for completion.
    // For now, start and hope it completes before the test ends (ideally sync would be better).
    app.startRender(params);

    // Poll until rendering completes (with timeout)
    int maxWaitMs = 30000;  // 30 second max wait
    auto start = std::chrono::steady_clock::now();
    while (app.isRenderingActive()) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
        if (elapsed > maxWaitMs) {
            qWarning() << "SRenderAction: render timeout after" << maxWaitMs << "ms";
            return {false, nullptr};
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    qDebug() << "SRenderAction: rendered to" << fullPath;
    return {true, nullptr};  // Renders are not undoable
}

void SRenderAction::writeXml(QDomElement &elem) const
{
    elem.setAttribute("filename", filename_);

    QString formatStr;
    switch (format_) {
        case Format::WAV:
            formatStr = "wav";
            break;
        case Format::OGG:
            formatStr = "ogg";
            break;
        case Format::MP3:
            formatStr = "mp3";
            break;
    }
    elem.setAttribute("format", formatStr);
    elem.setAttribute("quality", QString::number(quality_));
}

bool SRenderAction::readXml(const QDomElement &elem, int /*version*/)
{
    filename_ = elem.attribute("filename", "");
    // Note: empty filename is allowed for roundtrip testing; apply() will reject it

    QString formatStr = elem.attribute("format", "wav");
    if (formatStr == "wav") {
        format_ = Format::WAV;
    } else if (formatStr == "ogg") {
        format_ = Format::OGG;
    } else if (formatStr == "mp3") {
        format_ = Format::MP3;
    } else {
        qWarning() << "SRenderAction::readXml: unknown format:" << formatStr;
        return false;
    }

    bool ok;
    quality_ = elem.attribute("quality", "10").toInt(&ok);
    if (!ok || quality_ < 0 || quality_ > 320) {
        qWarning() << "SRenderAction::readXml: invalid quality:" << elem.attribute("quality");
        return false;
    }

    return true;
}

static const bool s_reg_render = (
    SActionRegistry::instance().registerType(
        QStringLiteral("render"),
        []{ return new SRenderAction; }
    ), true
);
