#include "app/actions/srenderaction.h"
#include "app/model/sappcontext.h"
#include "app/model/sproject.h"
#include "app/actions/sactionregistry.h"
#include "app/model/sobject.h"
#include "tw/render/render_session.h"
#include <QDomElement>
#include <QDebug>
#include <QFileInfo>
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
    std::shared_ptr<twComponent> synthOutput;
    SObject *root = project->getRootComponent();
    if (root) {
        synthOutput = root->getRootComponent();
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
    // The ARRANGEMENT decides how long this render is (it used to be a hardcoded
    // 60 s, which truncated anything longer and padded everything shorter),
    // unless the case pins an explicit durationSec.
    params.endTimeSec = ( durationSec_ > 0.0 ) ? durationSec_
                                               : project->getDurationSeconds();

    // Start rendering
    // Note: RenderSession is asynchronous. For test mode, we should wait for completion.
    // For now, start and hope it completes before the test ends (ideally sync would be better).
    app.startRender(params);

    // Poll until rendering completes, under a watchdog.
    //
    // The watchdog exists to stop a HUNG render from hanging the run forever --
    // it is not an assertion about how fast a render is. But it is a WALL-CLOCK
    // budget for the whole render, so it doubles as exactly such an assertion
    // the moment the machine is busy, and `ctest -j` makes the machine busy by
    // construction. Measured under `-j8`: a render advanced steadily, a page
    // every ~1.6 s instead of the usual ~0.03 s, and was killed at 96 % done
    // (2 764 800 of 2 880 000 samples) about a second from finishing. That is a
    // false failure on a working render, and with <render> appearing 147 times
    // across the qxa suite it is the single thing that decides how high `-j` can
    // go.
    //
    // So the budget is overridable. The default is unchanged, which keeps the
    // app's behaviour and any hand-run case exactly as before; the qxa suite
    // raises it (see smaragd/CMakeLists.txt) and relies on CTest's own per-test
    // TIMEOUT as the real hang guard -- a bound that is about the test, not
    // about one action inside it.
    // The default budget scales with the arrangement now that the extent is
    // not a fixed minute (PR #34): a case may legally render several minutes
    // of audio. SMARAGD_RENDER_TIMEOUT_MS, when set, replaces it outright.
    int maxWaitMs = 30000 + static_cast<int>(params.endTimeSec * 2000.0);
    {
        const QByteArray env = qgetenv("SMARAGD_RENDER_TIMEOUT_MS");
        bool ok = false;
        const int v = env.toInt(&ok);
        if (ok && v > 0) maxWaitMs = v;
    }
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

    // A render that never started leaves no file behind, and startRender() does
    // not report that back — the action would otherwise report SUCCESS for a
    // case whose output does not exist, and the failure would surface much later
    // as a confusing assert against a missing WAV.
    if (!QFileInfo::exists(fullPath)) {
        qWarning() << "SRenderAction: no output file was produced:" << fullPath;
        return {false, nullptr};
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
    // Written only when set, so every existing case round-trips unchanged.
    if (durationSec_ > 0.0) {
        elem.setAttribute("durationSec", QString::number(durationSec_));
    }
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

    durationSec_ = elem.attribute("durationSec", "-1").toDouble(&ok);
    if (!ok) {
        qWarning() << "SRenderAction::readXml: invalid durationSec:"
                   << elem.attribute("durationSec");
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
