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
    } else {
        qWarning() << "assert-groove-aspect: unknown aspect" << aspect_
                   << "(expected groove.res or groove.ev)";
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
