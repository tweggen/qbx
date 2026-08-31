#include "app/testkit/ssidecartestactions.h"
#include "app/actions/sactionregistry.h"
#include "app/shell/sapplication.h"
#include "app/shell/smainwindow.h"
#include "app/model/sproject.h"
#include "app/model/sexternfile.h"
#include "app/model/slink.h"
#include "app/model/splacements.h"
#include "app/model/sobjectpath.h"
#include "app/objects/cut/scut.h"
#include "app/objects/wave/splainwave.h"
#include "app/objects/track/strack.h"
#include "app/objects/track/strackpath.h"
#include "app/objects/track/sfeelflowbounce.h"
#include "app/objects/track/sfeelflowpose.h"
#include "app/model/sfeelflowskeleton.h"

#include "tw/schedule/capture_revalidator.h"
#include "tw/sidecar/twsidecarstore.h"
#include "tw/sidecar/twqaf.h"
#include "tw/sidecar/twgrooveaspect.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QDomElement>
#include <QElapsedTimer>
#include <QThread>
#include <QWidget>
#include <QDebug>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

using namespace strackpath;

namespace {

// Proposal 40 M1b: same track addressing (index-path from the root mixer)
// spluginuitestactions.cpp's own trackAtPath uses, kept as a small local
// duplicate rather than a shared refactor — the same call this file already
// makes twice below for two different reasons (starting a bounce, asserting
// staleness).
STrack *feelFlowTrackAt( SProject *project, const QString &trackPath )
{
    SObject *root = splacements::rootContainer( project );
    SObject *lane = splacements::laneAt( root, stringToPath( trackPath ) );
    return dynamic_cast<STrack *>( lane );
}

// Recursive tree walk for wait-analysis's M1b extension: true if ANY track
// under `node` (node itself included when it is a track) is still bouncing.
bool anyTrackBouncing( SObject *node )
{
    if( !node ) return false;
    if( STrack *t = dynamic_cast<STrack *>( node ) ) {
        if( t->isFeelFlowBouncing() ) return true;
    }
    for( SLink *lk : node->childLinks() ) {
        if( anyTrackBouncing( &lk->getSObject() ) ) return true;
    }
    return false;
}

} // namespace

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
                // isAnalyzingGroove() (proposal 40 M1) is a SEPARATE badge
                // from isAnalyzing() — the groove job is its own opt-in
                // background pass, not folded into the onsets/loudness/f0
                // one — so both must be drained before this verb reports
                // "no jobs pending".
                if (pw && (pw->isAnalyzing() || pw->isAnalyzingGroove())) {
                    pending = true;
                    break;
                }
            }
        }
        if (!pending) {
            // Proposal 40 M1b: a track bounce runs on ITS OWN thread, not a
            // queued revalidator job, so jobsQueued()==0 above does not mean
            // "no bounce in flight" — only that no ANALYSIS job is queued
            // yet (the bounce schedules one only on completion).
            pending = anyTrackBouncing(splacements::rootContainer(project));
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

    SObject *mixer = splacements::rootNamed( project, pathRoot_ );
    SLink *link = splacements::placementAt(mixer, clipPath_);
    if (!link) {
        qWarning() << "set-render-gate: no clip at path" << qualifiedToString( pathRoot_, clipPath_ );
        return {false, nullptr};
    }

    SCut *cut = dynamic_cast<SCut*>(&link->getSObject());
    if (!cut) {
        qWarning() << "set-render-gate: target is not an SCut at path"
                   << qualifiedToString( pathRoot_, clipPath_ );
        return {false, nullptr};
    }

    cut->setRenderGateReady(ready_);
    qDebug() << "set-render-gate: clip" << qualifiedToString( pathRoot_, clipPath_ )
             << "ready=" << ready_;
    return {true, nullptr};
}

void SSetRenderGateAction::writeXml(QDomElement &elem) const
{
    elem.setAttribute("clip", qualifiedToString( pathRoot_, clipPath_ ));
    elem.setAttribute("ready", ready_ ? "true" : "false");
}

bool SSetRenderGateAction::readXml(const QDomElement &elem, int /*version*/)
{
    clipPath_ = parseInto( pathRoot_, elem.attribute("clip") );
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

    SObject *mixer = splacements::rootNamed( project, pathRoot_ );
    SLink *link = splacements::placementAt(mixer, clipPath_);
    if (!link) {
        qWarning() << "assert-warp-anchor: no clip at path" << qualifiedToString( pathRoot_, clipPath_ );
        return {false, nullptr};
    }
    SCut *cut = dynamic_cast<SCut*>(&link->getSObject());
    if (!cut) {
        qWarning() << "assert-warp-anchor: target is not an SCut at path"
                   << qualifiedToString( pathRoot_, clipPath_ );
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

    qDebug() << "assert-warp-anchor: clip" << qualifiedToString( pathRoot_, clipPath_ )
             << "OK (" << (qulonglong)anchors.size() << "anchors)";
    return {true, nullptr};
}

void SAssertWarpAnchorAction::writeXml(QDomElement &elem) const
{
    elem.setAttribute("clip", qualifiedToString( pathRoot_, clipPath_ ));
    if (hasSrc_) elem.setAttribute("src", QString::number(src_));
    elem.setAttribute("warped", QString::number(warped_));
    elem.setAttribute("count", QString::number(count_));
}

bool SAssertWarpAnchorAction::readXml(const QDomElement &elem, int /*version*/)
{
    clipPath_ = parseInto( pathRoot_, elem.attribute("clip") );
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

// ------------------------------------------------------------ feel-flow-analyze

SApplyResult SFeelFlowAnalyzeAction::apply(SProject *project)
{
    if (!project) {
        qWarning() << "feel-flow-analyze: no project";
        return {false, nullptr};
    }

    if (target_ == QStringLiteral("track")) {
        STrack *track = feelFlowTrackAt(project, trackPath_);
        if (!track) {
            qWarning() << "feel-flow-analyze: no track at path" << trackPath_;
            return {false, nullptr};
        }
        track->startFeelFlowBounce();
        qDebug() << "feel-flow-analyze: track" << trackPath_ << "bounce scheduled";
        // Non-undoable: scheduling a background bounce+analysis is not an
        // edit to the arrangement (mirrors the clip-target path below).
        return {true, nullptr};
    }

    if (clipPath_.isEmpty()) {
        qWarning() << "feel-flow-analyze: empty clip path";
        return {false, nullptr};
    }

    SObject *mixer = splacements::rootContainer(project);
    SLink *link = splacements::placementAt(mixer, clipPath_);
    if (!link) {
        qWarning() << "feel-flow-analyze: no clip at path" << pathToString(clipPath_);
        return {false, nullptr};
    }

    SCut *cut = dynamic_cast<SCut*>(&link->getSObject());
    if (!cut) {
        qWarning() << "feel-flow-analyze: target is not an SCut at path"
                   << pathToString(clipPath_);
        return {false, nullptr};
    }

    SPlainWave *wave = dynamic_cast<SPlainWave*>(&cut->getContent());
    if (!wave) {
        qWarning() << "feel-flow-analyze: clip content at path"
                   << pathToString(clipPath_) << "is not a plain wave";
        return {false, nullptr};
    }

    wave->enqueueGrooveAnalysis();
    qDebug() << "feel-flow-analyze: clip" << pathToString(clipPath_) << "scheduled";
    // Non-undoable: scheduling a background analysis is not an edit to the
    // arrangement (mirrors sidecar-root / wait-analysis above).
    return {true, nullptr};
}

void SFeelFlowAnalyzeAction::writeXml(QDomElement &elem) const
{
    if (target_ == QStringLiteral("track")) {
        elem.setAttribute("target", "track");
        elem.setAttribute("trackPath", trackPath_);
        return;
    }
    // target="clip" is the implicit default (never written) so every
    // existing case and golden round-trips byte-unchanged.
    elem.setAttribute("clip", pathToString(clipPath_));
}

bool SFeelFlowAnalyzeAction::readXml(const QDomElement &elem, int /*version*/)
{
    target_ = elem.attribute("target", "clip");
    if (target_ == QStringLiteral("track")) {
        trackPath_ = elem.attribute("trackPath");
        return true;
    }
    clipPath_ = stringToPath(elem.attribute("clip"));
    return true;
}

static const bool s_reg_feel_flow_analyze = (
    SActionRegistry::instance().registerType(
        QStringLiteral("feel-flow-analyze"),
        []{ return new SFeelFlowAnalyzeAction; }
    ), true
);

// --------------------------------------------------------- assert-groove-aspect

namespace {

// Same newest-file convention as SAssertSidecarAction::apply above, kept as
// a separate (small, duplicated) helper rather than a shared refactor of
// that action's body — this verb's failure/found reporting differs enough
// (aspect-specific structural checks follow) that sharing the control flow
// was not worth risking that existing, gated action's behavior.
bool findNewestGrooveQaf(const std::filesystem::path &root, const QString &aspect,
                        std::filesystem::path &out)
{
    const std::string needle = "." + aspect.toStdString() + ".";
    bool found = false;
    std::filesystem::file_time_type newestTime{};

    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) return false;
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
            out = it->path();
            newestTime = t;
            found = true;
        }
    }
    return found;
}

// Same pick as groove_test.cc's own fx::lowHighRegions: "low" is the region
// whose muMs sits closest to zero, "high" is the region with the most
// POSITIVE muMs — a structural pick off the data, never a hardcoded region
// index (the front end's region count/edges are an analysis-side param).
void lowHighRegions(const twGrooveResidualReport &r, int &lowOut, int &highOut)
{
    lowOut = -1; highOut = -1;
    double bestLowAbsMu = 1e18, bestHighMu = -1e18;
    for (size_t i = 0; i < r.perRegion.size(); i++) {
        if (!r.perRegion[i].hasData) continue;
        const double mu = r.perRegion[i].muMs;
        if (std::fabs(mu) < bestLowAbsMu) { bestLowAbsMu = std::fabs(mu); lowOut = (int)i; }
        if (mu > bestHighMu) { bestHighMu = mu; highOut = (int)i; }
    }
}

} // namespace

SApplyResult SAssertGrooveAspectAction::apply(SProject *project)
{
    if (aspect_.isEmpty()) {
        qWarning() << "assert-groove-aspect: missing aspect";
        return {false, nullptr};
    }

    // Proposal 40 M1b: a track's staleness is a property of the HOLDER, not
    // of any file on disk — checked independently of the QAF glob below.
    if (!trackPath_.isEmpty() && stale_ >= 0) {
        if (!project) {
            qWarning() << "assert-groove-aspect: no project (trackPath given)";
            return {false, nullptr};
        }
        STrack *track = feelFlowTrackAt(project, trackPath_);
        if (!track) {
            qWarning() << "assert-groove-aspect: no track at path" << trackPath_;
            return {false, nullptr};
        }
        const bool isStale = track->feelFlowStale();
        const bool wantStale = (stale_ != 0);
        if (isStale != wantStale) {
            qWarning() << "assert-groove-aspect: track" << trackPath_
                       << "feelFlowStale()=" << isStale << "expected" << wantStale;
            return {false, nullptr};
        }
        qDebug() << "assert-groove-aspect: track" << trackPath_
                 << "feelFlowStale()=" << isStale << "OK";
    }

    SApplication &app = SApplication::app();
    QString outputDir = app.testOutputDir();
    if (outputDir.isEmpty()) {
        qWarning() << "assert-groove-aspect: no test output directory configured";
        return {false, nullptr};
    }

    const std::filesystem::path root = (outputDir + "/sidecars").toStdString();
    std::filesystem::path newest;
    if (!findNewestGrooveQaf(root, aspect_, newest)) {
        if (expectExists_) {
            qWarning() << "assert-groove-aspect: no" << aspect_
                       << "sidecar under" << QString::fromStdString(root.string());
            return {false, nullptr};
        }
        qDebug() << "assert-groove-aspect: no" << aspect_
                 << "sidecar (expectExists=false, OK)";
        return {true, nullptr};
    }

    twQafReader reader;
    if (!reader.open(newest)) {
        qWarning() << "assert-groove-aspect: failed to open"
                   << QString::fromStdString(newest.string());
        return {false, nullptr};
    }
    const twQafInfo &info = reader.info();
    const uint64_t recordCount = info.recordCount;
    if (minRecords_ >= 0 && (int64_t)recordCount < minRecords_) {
        qWarning() << "assert-groove-aspect:" << aspect_ << "recordCount"
                   << (qulonglong)recordCount << "below min" << (qlonglong)minRecords_;
        return {false, nullptr};
    }
    if (maxRecords_ >= 0 && (int64_t)recordCount > maxRecords_) {
        qWarning() << "assert-groove-aspect:" << aspect_ << "recordCount"
                   << (qulonglong)recordCount << "above max" << (qlonglong)maxRecords_;
        return {false, nullptr};
    }

    std::vector<uint8_t> payload;
    if (!reader.readAllPayload(payload)) {
        qWarning() << "assert-groove-aspect: readAllPayload failed for"
                   << QString::fromStdString(newest.string());
        return {false, nullptr};
    }

    if (aspect_ == QStringLiteral("groove.res")) {
        // nUnits is not in the QAF header (twaspects.h's "groove.res" doc):
        // derive it from this file's own geometry.
        const uint32_t nUnits = recordCount > 0
            ? (uint32_t)( info.recordStride / 4 - 1 )
            : 0;
        std::vector<twGrooveResRecord> decoded =
            twGrooveDecodeResPayload(payload.data(), payload.size(), nUnits);
        if (decoded.size() != recordCount) {
            qWarning() << "assert-groove-aspect: groove.res decode mismatch — got"
                       << (qulonglong)decoded.size() << "records, header says"
                       << (qulonglong)recordCount;
            return {false, nullptr};
        }
        for (const twGrooveResRecord &rec : decoded) {
            for (float p : rec.unitPower) {
                if (!(p >= 0.0f && p <= 1.0f)) {
                    qWarning() << "assert-groove-aspect: groove.res per-unit power"
                               << p << "outside [0,1]";
                    return {false, nullptr};
                }
            }
            if (!(rec.compliance >= 0.0f && rec.compliance <= 1.0f)) {
                qWarning() << "assert-groove-aspect: groove.res compliance"
                           << rec.compliance << "outside [0,1]";
                return {false, nullptr};
            }
        }
    } else if (aspect_ == QStringLiteral("groove.ev")) {
        std::vector<twGrooveEvRecord> decoded =
            twGrooveDecodeEvPayload(payload.data(), payload.size());
        if (decoded.size() != recordCount) {
            qWarning() << "assert-groove-aspect: groove.ev decode mismatch — got"
                       << (qulonglong)decoded.size() << "records, header says"
                       << (qulonglong)recordCount;
            return {false, nullptr};
        }
        uint64_t lastPos = 0;
        for (size_t i = 0; i < decoded.size(); i++) {
            if (i > 0 && decoded[i].pos < lastPos) {
                qWarning() << "assert-groove-aspect: groove.ev records not ascending by pos"
                           << "at index" << (qulonglong)i;
                return {false, nullptr};
            }
            lastPos = decoded[i].pos;
        }

        if (hasDeltaMu_) {
            uint16_t maxRegion = 0;
            for (const twGrooveEvRecord &e : decoded)
                maxRegion = std::max(maxRegion, e.region);
            std::vector<std::vector<twGrooveScoredEvent>> byRegion(
                (size_t)maxRegion + 1);
            const double rate = info.sourceRate > 0 ? (double)info.sourceRate : 48000.0;
            for (const twGrooveEvRecord &e : decoded)
                // amp uniform (this aspect carries no per-event amplitude —
                // see the header doc): equal amps mean the bleed gate never
                // fires, matching what M0 itself measured on this fixture
                // set (twGrooveStatsParams::bleedGateDb's doc).
                byRegion[e.region].push_back({ (double)e.pos / rate, (double)e.residualMs, 1.0f });
            const double totalSec = info.sourceFrames > 0
                ? (double)info.sourceFrames / rate
                : 0.0;
            twGrooveResidualReport report =
                twGroovePoolRegionStats(byRegion, totalSec, twGrooveStatsParams{});
            int lo = -1, hi = -1;
            lowHighRegions(report, lo, hi);
            if (lo < 0 || hi < 0 || lo == hi) {
                qWarning() << "assert-groove-aspect: groove.ev — could not find distinct"
                           << "low/high regions to measure deltaMu";
                return {false, nullptr};
            }
            const double deltaMu = report.perRegion[hi].muMs - report.perRegion[lo].muMs;
            const double diff = std::fabs(deltaMu - deltaMuHighLowMs_);
            qDebug() << "assert-groove-aspect: groove.ev deltaMu(high-low)=" << deltaMu
                     << "target=" << deltaMuHighLowMs_ << "tol=" << deltaMuTolMs_;
            if (diff > deltaMuTolMs_) {
                qWarning() << "assert-groove-aspect: groove.ev deltaMu(high-low)=" << deltaMu
                           << "outside" << deltaMuHighLowMs_ << "+/-" << deltaMuTolMs_;
                return {false, nullptr};
            }
        }
    } else if (aspect_ == QStringLiteral("groove.dyn")) {
        // Proposal 40 M3c; v3 since proposal 44 C1a added the two METRICAL
        // phase channels. nUnits from this file's own geometry
        // (recordStride = nUnits*8*4, twaspects.h).
        //
        // **THIS DIVISOR AND THE STORED STRIDE WERE BOTH LEFT AT v2's 6
        // FLOATS, AND THEY CANCELLED.** C1a grew the payload to 8 floats per
        // unit and updated neither the two store sites nor this line, so the
        // header advertised nUnits*24 bytes for a record that was really
        // nUnits*32 -- and 120/24 gives exactly the 5 units the fixture has,
        // so every assertion passed. Correcting either one ALONE breaks the
        // pair: with the stride fixed and this still at 24, 160/24 reads 6
        // units and the decode mismatches. Both are fixed together, which is
        // the only way this can be fixed at all.
        //
        // The stored stride was never harmless: twQafReader::readRecords()
        // seeks by first*stride, so any future caller addressing groove.dyn
        // by record would have read at 24-byte offsets into a 32-byte grid.
        // Latent only because every current reader takes readAllPayload().
        const uint32_t nUnits = recordCount > 0
            ? (uint32_t)( info.recordStride / 32 )
            : 0;
        std::vector<twGrooveDynRecord> decoded =
            twGrooveDecodeDynPayload(payload.data(), payload.size(), nUnits);
        if (decoded.size() != recordCount) {
            qWarning() << "assert-groove-aspect: groove.dyn decode mismatch — got"
                       << (qulonglong)decoded.size() << "records, header says"
                       << (qulonglong)recordCount;
            return {false, nullptr};
        }
        for (const twGrooveDynRecord &rec : decoded) {
            for (const twGrooveUnitDynSample &u : rec.units) {
                // support/tension are legitimately signed; slip and
                // dissipation are non-negative by construction.
                if (!(u.slip >= 0.0f) || !(u.dissip >= 0.0f)) {
                    qWarning() << "assert-groove-aspect: groove.dyn negative"
                               << "slip/dissip" << u.slip << u.dissip;
                    return {false, nullptr};
                }
                // M3e (v2): the phase channels are the bin-averaged
                // (cos,sin) pair, i.e. the circular-mean NUMERATOR, so
                // their magnitude can never exceed 1 -- and a magnitude
                // over 1 is the signature of a channel written at the
                // wrong offset (a stride/order mismatch), not of a
                // legitimate phase.
                const double mag = std::hypot((double)u.cosPhi,
                                              (double)u.sinPhi);
                if (!(mag <= 1.0001)) {
                    qWarning() << "assert-groove-aspect: groove.dyn"
                               << "hypot(cosPhi,sinPhi)" << mag << "> 1"
                               << u.cosPhi << u.sinPhi;
                    return {false, nullptr};
                }
            }
        }
    } else {
        qWarning() << "assert-groove-aspect: unknown aspect" << aspect_
                   << "(expected groove.res, groove.ev or groove.dyn)";
        return {false, nullptr};
    }

    qDebug() << "assert-groove-aspect:" << aspect_ << "recordCount" << (qulonglong)recordCount
             << "OK (" << QString::fromStdString(newest.filename().string()) << ")";
    return {true, nullptr};
}

void SAssertGrooveAspectAction::writeXml(QDomElement &elem) const
{
    elem.setAttribute("aspect", aspect_);
    elem.setAttribute("minRecords", QString::number(minRecords_));
    elem.setAttribute("maxRecords", QString::number(maxRecords_));
    elem.setAttribute("expectExists", expectExists_ ? "true" : "false");
    if (hasDeltaMu_) {
        elem.setAttribute("deltaMuHighLowMs", QString::number(deltaMuHighLowMs_));
        elem.setAttribute("deltaMuTolMs", QString::number(deltaMuTolMs_));
    }
    // Proposal 40 M1b: written only when used, so every M1-era case and
    // golden round-trips byte-unchanged.
    if (!trackPath_.isEmpty()) {
        elem.setAttribute("trackPath", trackPath_);
        elem.setAttribute("stale", QString::number(stale_));
    }
}

bool SAssertGrooveAspectAction::readXml(const QDomElement &elem, int /*version*/)
{
    aspect_ = elem.attribute("aspect", "");
    minRecords_ = elem.attribute("minRecords", "-1").toLongLong();
    maxRecords_ = elem.attribute("maxRecords", "-1").toLongLong();
    expectExists_ = elem.attribute("expectExists", "true") == "true";
    hasDeltaMu_ = elem.hasAttribute("deltaMuHighLowMs");
    deltaMuHighLowMs_ = elem.attribute("deltaMuHighLowMs", "0").toDouble();
    deltaMuTolMs_ = elem.attribute("deltaMuTolMs", "0").toDouble();
    trackPath_ = elem.attribute("trackPath", "");
    stale_ = elem.attribute("stale", "-1").toInt();
    return true;
}

static const bool s_reg_assert_groove_aspect = (
    SActionRegistry::instance().registerType(
        QStringLiteral("assert-groove-aspect"),
        []{ return new SAssertGrooveAspectAction; }
    ), true
);

// --------------------------------------------------- click-feel-flow-panel

namespace {
// testkit may not include app/timeline (CONTRACT inv. 5), so the section is
// reached through the shell, exactly as assert-track-head/collect-envelope
// do -- each of those files keeps its own file-local copy of this lookup.
SMainWindow *feelFlowMainWindow()
{
    for( QWidget *w : QApplication::topLevelWidgets() )
        if( SMainWindow *win = qobject_cast<SMainWindow *>( w ) ) return win;
    return nullptr;
}
} // namespace

SApplyResult SClickFeelFlowPanelAction::apply( SProject * )
{
    SMainWindow *win = feelFlowMainWindow();
    if( !win ) {
        qWarning() << "click-feel-flow-panel: no main window";
        return { false, nullptr };
    }
    if( !win->clickFeelFlowPanel( trackPath_, button_ ) ) {
        qWarning() << "click-feel-flow-panel FAILED: track" << trackPath_
                  << "button" << button_;
        return { false, nullptr };
    }
    return { true, nullptr };   // both routes it can drive are non-undoable
                                 // OR submit their own undoable action already
}

void SClickFeelFlowPanelAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", trackPath_ );
    elem.setAttribute( "button", button_ );
}

bool SClickFeelFlowPanelAction::readXml( const QDomElement &elem, int /*version*/ )
{
    trackPath_ = elem.attribute( "trackPath", "0" );
    button_    = elem.attribute( "button", "" );
    return true;
}

static const bool s_reg_click_feel_flow_panel = (
    SActionRegistry::instance().registerType(
        QStringLiteral("click-feel-flow-panel"),
        []{ return new SClickFeelFlowPanelAction; }
    ), true
);

// -------------------------------------------------- assert-feel-flow-panel

SApplyResult SAssertFeelFlowPanelAction::apply( SProject * )
{
    SMainWindow *win = feelFlowMainWindow();
    if( !win ) {
        qWarning() << "assert-feel-flow-panel: no main window";
        return { false, nullptr };
    }
    const QString desc = win->describeFeelFlow( trackPath_ );
    if( desc.isEmpty() ) {
        qWarning() << "assert-feel-flow-panel FAILED: no track at" << trackPath_;
        return { false, nullptr };
    }
    if( !contains_.isEmpty() && !desc.contains( contains_ ) ) {
        qWarning() << "assert-feel-flow-panel FAILED: reports" << desc
                  << "which lacks" << contains_;
        return { false, nullptr };
    }
    if( !absent_.isEmpty() && desc.contains( absent_ ) ) {
        qWarning() << "assert-feel-flow-panel FAILED: reports" << desc
                  << "which unexpectedly contains" << absent_;
        return { false, nullptr };
    }
    if( !grabPng_.isEmpty() ) {
        if( grabPng_.contains( '/' ) || grabPng_.contains( '\\' )
            || grabPng_.contains( ".." ) ) {
            qWarning() << "assert-feel-flow-panel: grabPng contains path"
                          " separators:" << grabPng_;
            return { false, nullptr };
        }
        SApplication &app = SApplication::app();
        if( app.testOutputDir().isEmpty() || !app.ensureOutputDirExists() ) {
            qWarning() << "assert-feel-flow-panel FAILED: no usable test"
                          " output directory";
            return { false, nullptr };
        }
        const QString out = QDir( app.testOutputDir() ).filePath( grabPng_ );
        if( !win->grabFeelFlow( out, trackPath_, grabWidth_, grabHeight_ ) ) {
            qWarning() << "assert-feel-flow-panel FAILED: could not grab the"
                          " panel into" << out;
            return { false, nullptr };
        }
    }
    qDebug().noquote() << "assert-feel-flow-panel: OK -" << desc;
    return { true, nullptr };
}

QStringList SAssertFeelFlowPanelAction::knownAttributes() const
{
    return { QStringLiteral("trackPath"), QStringLiteral("contains"),
             QStringLiteral("absent"), QStringLiteral("grabPng"),
             QStringLiteral("grabWidth"), QStringLiteral("grabHeight") };
}

void SAssertFeelFlowPanelAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", trackPath_ );
    if( !contains_.isEmpty() ) elem.setAttribute( "contains", contains_ );
    if( !absent_.isEmpty() )   elem.setAttribute( "absent", absent_ );
    if( !grabPng_.isEmpty() ) {
        elem.setAttribute( "grabPng", grabPng_ );
        if( grabWidth_ > 0 )  elem.setAttribute( "grabWidth", grabWidth_ );
        if( grabHeight_ > 0 ) elem.setAttribute( "grabHeight", grabHeight_ );
    }
}

bool SAssertFeelFlowPanelAction::readXml( const QDomElement &elem, int /*version*/ )
{
    trackPath_  = elem.attribute( "trackPath", "0" );
    contains_   = elem.attribute( "contains", "" );
    absent_     = elem.attribute( "absent", "" );
    grabPng_    = elem.attribute( "grabPng", "" );
    grabWidth_  = elem.attribute( "grabWidth", "0" ).toInt();
    grabHeight_ = elem.attribute( "grabHeight", "0" ).toInt();
    return true;
}

static const bool s_reg_assert_feel_flow_panel = (
    SActionRegistry::instance().registerType(
        QStringLiteral("assert-feel-flow-panel"),
        []{ return new SAssertFeelFlowPanelAction; }
    ), true
);

// ------------------------------------------------------ set-feel-flow-metric

SApplyResult SSetFeelFlowMetricAction::apply( SProject *project )
{
    STrack *track = feelFlowTrackAt( project, trackPath_ );
    if( !track ) {
        qWarning() << "set-feel-flow-metric: no STrack at" << trackPath_;
        return { false, nullptr };
    }
    // The SAME plain call the panel's combo makes (M3b): a runtime view
    // preference, never an action's edit. An unknown id is accepted -- the
    // band falls back to compliance at paint time by design.
    track->setFeelFlowBandMetricId( metric_.toStdString() );
    qDebug() << "set-feel-flow-metric: track" << trackPath_ << "metric"
             << QString::fromStdString( track->feelFlowBandMetricId() );
    return { true, nullptr };
}

void SSetFeelFlowMetricAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", trackPath_ );
    elem.setAttribute( "metric", metric_ );
}

bool SSetFeelFlowMetricAction::readXml( const QDomElement &elem, int /*version*/ )
{
    trackPath_ = elem.attribute( "trackPath", "0" );
    metric_    = elem.attribute( "metric", "" );
    return true;
}

static const bool s_reg_set_feel_flow_metric = (
    SActionRegistry::instance().registerType(
        QStringLiteral("set-feel-flow-metric"),
        []{ return new SSetFeelFlowMetricAction; }
    ), true
);

// ----------------------------------------------------- assert-feel-flow-pose

SApplyResult SAssertFeelFlowPoseAction::apply( SProject *project )
{
    STrack *track = feelFlowTrackAt( project, trackPath_ );
    if( !track ) {
        qWarning() << "assert-feel-flow-pose: no STrack at" << trackPath_;
        return { false, nullptr };
    }

    // The paint path's rule, mirrored: STALE yields the INVALID pose whatever
    // is cached. Decided HERE (and in SMainWindow::updateFeelFlowPuppet_),
    // never inside sFeelFlowPoseAt -- the pose function never sees a track.
    SFeelFlowPose pose;
    if( !track->feelFlowStale() ) {
        if( std::shared_ptr<const SFeelFlowUiData> data = track->feelFlowForUi() )
            pose = sFeelFlowPoseAt( *data, (offset_t) frame_ );
    }
    const QString desc =
        QString::fromStdString( sFeelFlowPoseDescribe( pose ) );

    // Always logged: this line is how a case MEASURES a bound before pinning
    // it (the house's measured-then-pinned rule).
    qDebug().noquote() << "assert-feel-flow-pose: track" << trackPath_
                       << "frame" << frame_ << "-" << desc;

    if( valid_ >= 0 && ( pose.valid ? 1 : 0 ) != valid_ ) {
        qWarning() << "assert-feel-flow-pose FAILED: valid is"
                   << ( pose.valid ? 1 : 0 ) << "expected" << valid_
                   << "-" << desc;
        return { false, nullptr };
    }

    const double absSum = std::fabs( (double) pose.bounceY )
                        + std::fabs( (double) pose.sway )
                        + std::fabs( (double) pose.armSwing )
                        + std::fabs( (double) pose.headNod )
                        + std::fabs( (double) pose.hipShift );
    if( minAbsSum_ >= 0.0 && absSum < minAbsSum_ ) {
        qWarning() << "assert-feel-flow-pose FAILED: absSum" << absSum
                   << "below minAbsSum" << minAbsSum_ << "-" << desc;
        return { false, nullptr };
    }
    if( maxAbsSum_ >= 0.0 && absSum > maxAbsSum_ ) {
        qWarning() << "assert-feel-flow-pose FAILED: absSum" << absSum
                   << "above maxAbsSum" << maxAbsSum_ << "-" << desc;
        return { false, nullptr };
    }
    if( !contains_.isEmpty() && !desc.contains( contains_ ) ) {
        qWarning() << "assert-feel-flow-pose FAILED: reports" << desc
                   << "which lacks" << contains_;
        return { false, nullptr };
    }

    if( !grabPng_.isEmpty() ) {
        if( grabPng_.contains( '/' ) || grabPng_.contains( '\\' )
            || grabPng_.contains( ".." ) ) {
            qWarning() << "assert-feel-flow-pose: grabPng contains path"
                          " separators:" << grabPng_;
            return { false, nullptr };
        }
        SApplication &app = SApplication::app();
        if( app.testOutputDir().isEmpty() || !app.ensureOutputDirExists() ) {
            qWarning() << "assert-feel-flow-pose FAILED: no usable test"
                          " output directory";
            return { false, nullptr };
        }
        SMainWindow *win = feelFlowMainWindow();
        if( !win ) {
            qWarning() << "assert-feel-flow-pose: no main window (grabPng)";
            return { false, nullptr };
        }
        const QString out = QDir( app.testOutputDir() ).filePath( grabPng_ );
        if( !win->grabFeelFlowPuppet( out, trackPath_, (offset_t) frame_,
                                      grabWidth_, grabHeight_ ) ) {
            qWarning() << "assert-feel-flow-pose FAILED: could not grab the"
                          " puppet into" << out;
            return { false, nullptr };
        }
    }
    return { true, nullptr };
}

QStringList SAssertFeelFlowPoseAction::knownAttributes() const
{
    return { QStringLiteral("trackPath"), QStringLiteral("frame"),
             QStringLiteral("valid"), QStringLiteral("minAbsSum"),
             QStringLiteral("maxAbsSum"), QStringLiteral("contains"),
             QStringLiteral("grabPng"), QStringLiteral("grabWidth"),
             QStringLiteral("grabHeight") };
}

void SAssertFeelFlowPoseAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", trackPath_ );
    elem.setAttribute( "frame", QString::number( frame_ ) );
    elem.setAttribute( "valid", valid_ );
    elem.setAttribute( "minAbsSum", QString::number( minAbsSum_, 'g', 10 ) );
    elem.setAttribute( "maxAbsSum", QString::number( maxAbsSum_, 'g', 10 ) );
    if( !contains_.isEmpty() ) elem.setAttribute( "contains", contains_ );
    if( !grabPng_.isEmpty() ) {
        elem.setAttribute( "grabPng", grabPng_ );
        if( grabWidth_ > 0 )  elem.setAttribute( "grabWidth", grabWidth_ );
        if( grabHeight_ > 0 ) elem.setAttribute( "grabHeight", grabHeight_ );
    }
}

bool SAssertFeelFlowPoseAction::readXml( const QDomElement &elem, int /*version*/ )
{
    trackPath_  = elem.attribute( "trackPath", "0" );
    frame_      = elem.attribute( "frame", "0" ).toLongLong();
    valid_      = elem.attribute( "valid", "-1" ).toInt();
    minAbsSum_  = elem.attribute( "minAbsSum", "-1" ).toDouble();
    maxAbsSum_  = elem.attribute( "maxAbsSum", "-1" ).toDouble();
    contains_   = elem.attribute( "contains", "" );
    grabPng_    = elem.attribute( "grabPng", "" );
    grabWidth_  = elem.attribute( "grabWidth", "0" ).toInt();
    grabHeight_ = elem.attribute( "grabHeight", "0" ).toInt();
    return true;
}

// -------------------------------------------------- assert-puppet-skeleton

SApplyResult SAssertPuppetSkeletonAction::apply( SProject * /*project*/ )
{
    SFeelFlowJoints j;
    j.sway     = (float) sway_;
    j.headNod  = (float) nod_;
    j.armSwing = (float) arm_;
    j.bounceY  = (float) bounce_;
    j.hipShift = (float) hip_;
    j.trunkFlex  = (float) flex_;
    // WHICH PLANE `sway` acts in. Defaults to the project-wide decision
    // (sfeelflowskel::kTrunkSagittal); `plane="lateral"` is what keeps the 3D
    // cross-term pins alive, since with sway sagittal it is coplanar with
    // trunkFlex and the pair merely adds.
    if( plane_ == QLatin1String( "sagittal" ) )     j.sagittalTrunk = true;
    else if( plane_ == QLatin1String( "lateral" ) ) j.sagittalTrunk = false;
    j.trunkTwist = (float) twist_;

    const SFeelFlowSkeleton sk =
        sFeelFlowSkeletonFor( j, QRectF( 0.0, 0.0, boxW_, boxH_ ) );
    const QString desc = sFeelFlowSkeletonDescribe( sk );

    // Always logged: this line is how a case MEASURES before it pins.
    qDebug().noquote() << "assert-puppet-skeleton: sway" << sway_ << "nod" << nod_
                       << "arm" << arm_ << "-" << desc;

    // Validity is ASSERTED, not assumed: a box too small to lay out is a real,
    // reachable answer (the widget returns early on it) and a case must be able
    // to pin that rather than only the happy path.
    if( ( sk.valid ? 1 : 0 ) != expectValid_ ) {
        qWarning() << "assert-puppet-skeleton FAILED: valid is"
                   << ( sk.valid ? 1 : 0 ) << "expected" << expectValid_
                   << "for box" << boxW_ << "x" << boxH_ << "-" << desc;
        return { false, nullptr };
    }
    if( !sk.valid ) {
        // Nothing else is meaningful; the describe line is still checkable.
        if( !contains_.isEmpty() && !desc.contains( contains_ ) ) {
            qWarning() << "assert-puppet-skeleton FAILED: reports" << desc
                       << "which lacks" << contains_;
            return { false, nullptr };
        }
        return { true, nullptr };
    }

    struct Expect { const char *what; double got; double want; };
    const Expect expects[] = {
        { "trunk",     sk.trunkLeanDeg, expectTrunk_  },
        { "trunkFlex", sk.trunkFlexDeg, expectFlex_   },
        { "armSwingL", sk.armSwingLDeg, expectSwingL_ },
        { "twist",     sk.shoulderTwistDeg, expectTwist_ },
    };
    for( const Expect &e : expects ) {
        if( e.want > -999.0 && std::fabs( e.got - e.want ) > tol_ ) {
            qWarning() << "assert-puppet-skeleton FAILED:" << e.what << e.got
                       << "expected" << e.want << "tol" << tol_ << "-" << desc;
            return { false, nullptr };
        }
    }

    // THE C0 ASSERTION. Every segment above the neck must carry the trunk's
    // lean. Only meaningful with nod/arm at 0, which the case supplies.
    if( inheritTol_ >= 0.0 ) {
        // PLANE-AWARE since proposal 44 C7. The `sway` scalar drives whichever
        // plane sfeelflowskel::kTrunkSagittal names, so the chain has to be
        // asserted in THAT plane -- comparing frontal readouts against a
        // frontal trunk while the trunk is driven sagittally is 0 against 0,
        // and eight rows of this case would have passed measuring nothing.
        //
        // NO SAGITTAL SHOULDER-BAR ROW, and that is geometry rather than an
        // omission: the bar lies ALONG the axis sagittal flexion rotates
        // about, so a forward lean carries it forward without turning it.
        struct Row { const char *what; double got; };
        const bool sag = j.sagittalTrunk;
        const double trunk = sag ? sk.trunkFlexDeg : sk.trunkLeanDeg;
        const Row sagRows[] = {
            { "headStubFlex", sk.headStubFlexDeg },
            { "armSwingL",    sk.armSwingLDeg    },
            { "armSwingR",    sk.armSwingRDeg    },
        };
        const Row latRows[] = {
            { "headStub",    sk.headStubLeanDeg },
            { "shoulderBar", sk.shoulderBarDeg  },
            { "armL",        sk.armLeanLDeg     },
            { "armR",        sk.armLeanRDeg     },
        };
        const Row *rows = sag ? sagRows : latRows;
        const int  nRows = sag ? 3 : 4;
        for( int i = 0; i < nRows; i++ ) {
            if( std::fabs( rows[i].got - trunk ) > inheritTol_ ) {
                qWarning() << "assert-puppet-skeleton FAILED:" << rows[i].what
                           << rows[i].got << "does not carry the trunk's"
                           << trunk << "within" << inheritTol_
                           << "-" << desc;
                return { false, nullptr };
            }
        }
        // AND THE OTHER PLANES STAY AT ZERO.
        //
        // **NOT "strictly stronger", and that claim was made here and then
        // measured false.** A 0.4-degree lateral leak injected on a sagittal
        // command is caught with this check AND without it, because the
        // explicit three-axis rows in section (3) of the case already pin all
        // three readouts for those commands. What this adds is coverage on the
        // inheritTol rows, which otherwise assert ONE axis: a leak appearing
        // only at 0.5 or at a small box would have no other assertion to trip.
        // Contrived, cheap, and kept on that basis rather than on a stronger
        // one.
        const double offA = sag ? sk.trunkLeanDeg : sk.trunkFlexDeg;
        if( std::fabs( offA ) > inheritTol_ || std::fabs( sk.shoulderTwistDeg ) > inheritTol_ ) {
            qWarning() << "assert-puppet-skeleton FAILED: a lean leaked into a"
                          " plane nothing commanded - off-plane" << offA
                       << "twist" << sk.shoulderTwistDeg << "-" << desc;
            return { false, nullptr };
        }
    }

    if( !contains_.isEmpty() && !desc.contains( contains_ ) ) {
        qWarning() << "assert-puppet-skeleton FAILED: reports" << desc
                   << "which lacks" << contains_;
        return { false, nullptr };
    }
    return { true, nullptr };
}

QStringList SAssertPuppetSkeletonAction::knownAttributes() const
{
    return { QStringLiteral("sway"), QStringLiteral("nod"), QStringLiteral("arm"),
             QStringLiteral("bounce"), QStringLiteral("hip"),
             QStringLiteral("flex"), QStringLiteral("twist"),
             QStringLiteral("expectFlex"), QStringLiteral("expectSwingL"),
             QStringLiteral("expectTwist"),
             QStringLiteral("boxW"), QStringLiteral("boxH"),
             QStringLiteral("expectValid"), QStringLiteral("expectTrunk"),
             QStringLiteral("inheritTol"),
             QStringLiteral("tol"), QStringLiteral("contains"),
             QStringLiteral("plane") };
}

void SAssertPuppetSkeletonAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "sway",   QString::number( sway_,   'g', 10 ) );
    elem.setAttribute( "nod",    QString::number( nod_,    'g', 10 ) );
    elem.setAttribute( "arm",    QString::number( arm_,    'g', 10 ) );
    elem.setAttribute( "bounce", QString::number( bounce_, 'g', 10 ) );
    elem.setAttribute( "hip",    QString::number( hip_,    'g', 10 ) );
    elem.setAttribute( "flex",   QString::number( flex_,   'g', 10 ) );
    elem.setAttribute( "twist",  QString::number( twist_,  'g', 10 ) );
    elem.setAttribute( "expectFlex",   QString::number( expectFlex_,   'g', 10 ) );
    elem.setAttribute( "expectSwingL", QString::number( expectSwingL_, 'g', 10 ) );
    elem.setAttribute( "expectTwist",  QString::number( expectTwist_,  'g', 10 ) );
    elem.setAttribute( "boxW",   QString::number( boxW_,   'g', 10 ) );
    elem.setAttribute( "boxH",   QString::number( boxH_,   'g', 10 ) );
    elem.setAttribute( "expectValid", expectValid_ );
    elem.setAttribute( "expectTrunk", QString::number( expectTrunk_, 'g', 10 ) );
    elem.setAttribute( "inheritTol",  QString::number( inheritTol_,  'g', 10 ) );
    elem.setAttribute( "tol",         QString::number( tol_,         'g', 10 ) );
    if( !plane_.isEmpty() )    elem.setAttribute( "plane", plane_ );
    if( !contains_.isEmpty() ) elem.setAttribute( "contains", contains_ );
}

bool SAssertPuppetSkeletonAction::readXml( const QDomElement &elem, int /*version*/ )
{
    sway_        = elem.attribute( "sway",   "0" ).toDouble();
    nod_         = elem.attribute( "nod",    "0" ).toDouble();
    arm_         = elem.attribute( "arm",    "0" ).toDouble();
    bounce_      = elem.attribute( "bounce", "0" ).toDouble();
    hip_         = elem.attribute( "hip",    "0" ).toDouble();
    flex_        = elem.attribute( "flex",   "0" ).toDouble();
    twist_       = elem.attribute( "twist",  "0" ).toDouble();
    expectFlex_   = elem.attribute( "expectFlex",   "-1000" ).toDouble();
    expectSwingL_ = elem.attribute( "expectSwingL", "-1000" ).toDouble();
    expectTwist_  = elem.attribute( "expectTwist",  "-1000" ).toDouble();
    boxW_        = elem.attribute( "boxW",   "200" ).toDouble();
    boxH_        = elem.attribute( "boxH",   "400" ).toDouble();
    expectValid_ = elem.attribute( "expectValid", "1" ).toInt();
    expectTrunk_ = elem.attribute( "expectTrunk", "-1000" ).toDouble();
    plane_       = elem.attribute( "plane", "" );
    inheritTol_  = elem.attribute( "inheritTol",  "-1" ).toDouble();
    tol_         = elem.attribute( "tol",         "0.01" ).toDouble();
    contains_    = elem.attribute( "contains", "" );
    return true;
}

static const bool s_reg_assert_puppet_skeleton = (
    SActionRegistry::instance().registerType(
        QStringLiteral("assert-puppet-skeleton"),
        []{ return new SAssertPuppetSkeletonAction; }
    ), true
);

static const bool s_reg_assert_feel_flow_pose = (
    SActionRegistry::instance().registerType(
        QStringLiteral("assert-feel-flow-pose"),
        []{ return new SAssertFeelFlowPoseAction; }
    ), true
);
