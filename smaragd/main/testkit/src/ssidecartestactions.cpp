#include "app/testkit/ssidecartestactions.h"
#include "app/actions/sactionregistry.h"
#include "app/shell/sapplication.h"
#include "app/model/sproject.h"
#include "app/model/sexternfile.h"
#include "app/model/slink.h"
#include "app/model/splacements.h"
#include "app/model/sobjectpath.h"
#include "app/objects/cut/scut.h"
#include "app/objects/wave/splainwave.h"

#include "tw/schedule/capture_revalidator.h"
#include "tw/sidecar/twsidecarstore.h"
#include "tw/sidecar/twqaf.h"

#include <QCoreApplication>
#include <QDomElement>
#include <QElapsedTimer>
#include <QThread>
#include <QDebug>

#include <filesystem>
#include <string>
#include <system_error>

using namespace strackpath;

// ---------------------------------------------------------------- sidecar-root

SApplyResult SSidecarRootAction::apply(SProject * /*project*/)
{
    SApplication &app = SApplication::app();
    QString outputDir = app.testOutputDir();
    if (outputDir.isEmpty()) {
        qWarning() << "sidecar-root: no test output directory configured";
        return {false, nullptr};
    }

    const QString root = outputDir + "/sidecars";
    twSidecarStore::instance().setRoot(root.toStdString());
    qDebug() << "sidecar-root: store root ->" << root;
    return {true, nullptr};
}

void SSidecarRootAction::writeXml(QDomElement & /*elem*/) const {}

bool SSidecarRootAction::readXml(const QDomElement & /*elem*/, int /*version*/)
{
    return true;
}

static const bool s_reg_sidecar_root = (
    SActionRegistry::instance().registerType(
        QStringLiteral("sidecar-root"),
        []{ return new SSidecarRootAction; }
    ), true
);

// --------------------------------------------------------------- wait-analysis

SApplyResult SWaitAnalysisAction::apply(SProject *project)
{
    if (!project) {
        return {false, nullptr};
    }

    // Null revalidator (SMARAGD_REVAL_WORKERS=0): analysis never enqueues and
    // no wave ever flags isAnalyzing() — nothing is pending, pass at once.
    CaptureRevalidator *reval = project->getRevalidator();

    QElapsedTimer timer;
    timer.start();

    for (;;) {
        bool pending = false;

        if (reval && reval->jobsQueued() != 0) {
            pending = true;
        }
        if (!pending) {
            for (SExternFile *ef : project->externFiles().values()) {
                SPlainWave *pw = dynamic_cast<SPlainWave*>(ef);
                if (pw && pw->isAnalyzing()) {
                    pending = true;
                    break;
                }
            }
        }

        if (!pending) {
            qDebug() << "wait-analysis: no jobs pending after"
                     << timer.elapsed() << "ms (OK)";
            return {true, nullptr};
        }

        if (timer.elapsed() > timeoutMs_) {
            qWarning() << "wait-analysis: TIMEOUT after" << timeoutMs_ << "ms;"
                       << "jobsQueued=" << (reval ? (qulonglong)reval->jobsQueued() : 0);
            return {false, nullptr};
        }

        QCoreApplication::processEvents();
        QThread::msleep(20);
    }
}

void SWaitAnalysisAction::writeXml(QDomElement &elem) const
{
    elem.setAttribute("timeoutMs", QString::number(timeoutMs_));
}

bool SWaitAnalysisAction::readXml(const QDomElement &elem, int /*version*/)
{
    timeoutMs_ = elem.attribute("timeoutMs", "15000").toInt();
    if (timeoutMs_ < 0) timeoutMs_ = 15000;
    return true;
}

static const bool s_reg_wait_analysis = (
    SActionRegistry::instance().registerType(
        QStringLiteral("wait-analysis"),
        []{ return new SWaitAnalysisAction; }
    ), true
);

// -------------------------------------------------------------- set-render-gate

SApplyResult SSetRenderGateAction::apply(SProject *project)
{
    if (!project || clipPath_.isEmpty()) {
        return {false, nullptr};
    }

    SObject *mixer = splacements::rootContainer(project);
    SLink *link = splacements::placementAt(mixer, clipPath_);
    if (!link) {
        qWarning() << "set-render-gate: no clip at path" << pathToString(clipPath_);
        return {false, nullptr};
    }

    SCut *cut = dynamic_cast<SCut*>(&link->getSObject());
    if (!cut) {
        qWarning() << "set-render-gate: target is not an SCut at path"
                   << pathToString(clipPath_);
        return {false, nullptr};
    }

    cut->setRenderGateReady(ready_);
    qDebug() << "set-render-gate: clip" << pathToString(clipPath_)
             << "ready=" << ready_;
    return {true, nullptr};
}

void SSetRenderGateAction::writeXml(QDomElement &elem) const
{
    elem.setAttribute("clip", pathToString(clipPath_));
    elem.setAttribute("ready", ready_ ? "true" : "false");
}

bool SSetRenderGateAction::readXml(const QDomElement &elem, int /*version*/)
{
    clipPath_ = stringToPath(elem.attribute("clip"));
    ready_ = elem.attribute("ready", "true") == "true";
    return true;
}

static const bool s_reg_set_render_gate = (
    SActionRegistry::instance().registerType(
        QStringLiteral("set-render-gate"),
        []{ return new SSetRenderGateAction; }
    ), true
);

// --------------------------------------------------------------- assert-sidecar

SApplyResult SAssertSidecarAction::apply(SProject * /*project*/)
{
    if (aspect_.isEmpty()) {
        qWarning() << "assert-sidecar: missing aspect";
        return {false, nullptr};
    }

    SApplication &app = SApplication::app();
    QString outputDir = app.testOutputDir();
    if (outputDir.isEmpty()) {
        qWarning() << "assert-sidecar: no test output directory configured";
        return {false, nullptr};
    }

    const std::filesystem::path root =
        (outputDir + "/sidecars").toStdString();

    // File name layout: <contenthash>.<aspect>.<paramshash>.qaf — match the
    // aspect token exactly and pick the newest file (LRU-touched on hit, but
    // freshly written wins here).
    const std::string needle = "." + aspect_.toStdString() + ".";
    std::filesystem::path newest;
    std::filesystem::file_time_type newestTime{};
    bool found = false;

    std::error_code ec;
    if (std::filesystem::exists(root, ec)) {
        for (std::filesystem::recursive_directory_iterator
                 it(root, ec), end; it != end; it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file(ec)) continue;
            const std::string fn = it->path().filename().string();
            if (fn.size() < 4 || fn.substr(fn.size() - 4) != ".qaf") continue;
            if (fn.find(needle) == std::string::npos) continue;

            const auto t = std::filesystem::last_write_time(it->path(), ec);
            if (ec) continue;
            if (!found || t > newestTime) {
                newest = it->path();
                newestTime = t;
                found = true;
            }
        }
    }

    if (!found) {
        if (expectExists_) {
            qWarning() << "assert-sidecar: no" << aspect_
                       << "sidecar under" << QString::fromStdString(root.string());
            return {false, nullptr};
        }
        qDebug() << "assert-sidecar: no" << aspect_
                 << "sidecar (expectExists=false, OK)";
        return {true, nullptr};
    }

    twQafReader reader;
    if (!reader.open(newest)) {
        qWarning() << "assert-sidecar: failed to open"
                   << QString::fromStdString(newest.string());
        return {false, nullptr};
    }

    const uint64_t recordCount = reader.info().recordCount;
    if (minRecords_ >= 0 && (int64_t)recordCount < minRecords_) {
        qWarning() << "assert-sidecar:" << aspect_ << "recordCount" << (qulonglong)recordCount
                   << "below min" << (qlonglong)minRecords_;
        return {false, nullptr};
    }
    if (maxRecords_ >= 0 && (int64_t)recordCount > maxRecords_) {
        qWarning() << "assert-sidecar:" << aspect_ << "recordCount" << (qulonglong)recordCount
                   << "above max" << (qlonglong)maxRecords_;
        return {false, nullptr};
    }

    qDebug() << "assert-sidecar:" << aspect_ << "recordCount" << (qulonglong)recordCount
             << "OK (" << QString::fromStdString(newest.filename().string()) << ")";
    return {true, nullptr};
}

void SAssertSidecarAction::writeXml(QDomElement &elem) const
{
    elem.setAttribute("aspect", aspect_);
    elem.setAttribute("minRecords", QString::number(minRecords_));
    elem.setAttribute("maxRecords", QString::number(maxRecords_));
    elem.setAttribute("expectExists", expectExists_ ? "true" : "false");
}

bool SAssertSidecarAction::readXml(const QDomElement &elem, int /*version*/)
{
    // A missing aspect is reported by apply() (which already guards it), not
    // here: readXml failing makes the action undeserializable, which breaks
    // the round-trip audit (it feeds every action a DEFAULT instance through
    // write→read→write).
    aspect_ = elem.attribute("aspect", "");
    minRecords_ = elem.attribute("minRecords", "-1").toLongLong();
    maxRecords_ = elem.attribute("maxRecords", "-1").toLongLong();
    expectExists_ = elem.attribute("expectExists", "true") == "true";
    return true;
}

static const bool s_reg_assert_sidecar = (
    SActionRegistry::instance().registerType(
        QStringLiteral("assert-sidecar"),
        []{ return new SAssertSidecarAction; }
    ), true
);

// ------------------------------------------------------------ assert-warp-anchor

SApplyResult SAssertWarpAnchorAction::apply(SProject *project)
{
    if (!project || clipPath_.isEmpty()) {
        qWarning() << "assert-warp-anchor: no project or empty clip path";
        return {false, nullptr};
    }

    SObject *mixer = splacements::rootContainer(project);
    SLink *link = splacements::placementAt(mixer, clipPath_);
    if (!link) {
        qWarning() << "assert-warp-anchor: no clip at path" << pathToString(clipPath_);
        return {false, nullptr};
    }
    SCut *cut = dynamic_cast<SCut*>(&link->getSObject());
    if (!cut) {
        qWarning() << "assert-warp-anchor: target is not an SCut at path"
                   << pathToString(clipPath_);
        return {false, nullptr};
    }

    const std::vector<twWarpAnchor> &anchors = cut->getGrainParams().warpAnchors;

    if (count_ >= 0 && (int64_t)anchors.size() != count_) {
        qWarning() << "assert-warp-anchor: anchor count" << (qulonglong)anchors.size()
                   << "!= expected" << (qlonglong)count_;
        return {false, nullptr};
    }

    if (hasSrc_) {
        const twWarpAnchor *found = nullptr;
        for (const twWarpAnchor &a : anchors)
            if (a.src == src_) { found = &a; break; }
        if (!found) {
            qWarning() << "assert-warp-anchor: no anchor at src" << (qlonglong)src_;
            return {false, nullptr};
        }
        if (warped_ >= 0 && found->warped != warped_) {
            qWarning() << "assert-warp-anchor: anchor src" << (qlonglong)src_
                       << "warped" << (qlonglong)found->warped
                       << "!= expected" << (qlonglong)warped_;
            return {false, nullptr};
        }
    }

    qDebug() << "assert-warp-anchor: clip" << pathToString(clipPath_)
             << "OK (" << (qulonglong)anchors.size() << "anchors)";
    return {true, nullptr};
}

void SAssertWarpAnchorAction::writeXml(QDomElement &elem) const
{
    elem.setAttribute("clip", pathToString(clipPath_));
    if (hasSrc_) elem.setAttribute("src", QString::number(src_));
    elem.setAttribute("warped", QString::number(warped_));
    elem.setAttribute("count", QString::number(count_));
}

bool SAssertWarpAnchorAction::readXml(const QDomElement &elem, int /*version*/)
{
    clipPath_ = stringToPath(elem.attribute("clip"));
    hasSrc_   = elem.hasAttribute("src");
    src_      = elem.attribute("src", "0").toLongLong();
    warped_   = elem.attribute("warped", "-1").toLongLong();
    count_    = elem.attribute("count", "-1").toLongLong();
    return true;
}

static const bool s_reg_assert_warp_anchor = (
    SActionRegistry::instance().registerType(
        QStringLiteral("assert-warp-anchor"),
        []{ return new SAssertWarpAnchorAction; }
    ), true
);
