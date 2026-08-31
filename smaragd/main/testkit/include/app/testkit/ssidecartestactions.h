#ifndef SSIDECARTESTACTIONS_H
#define SSIDECARTESTACTIONS_H

#include "app/actions/saction.h"

#include <QList>

// Proposal 27 M1 testkit verbs for the derived-data sidecar substrate:
//
//   <sidecar-root/>
//       Points the process-global twSidecarStore at
//       <testOutputDir>/sidecars, so each headless case is hermetic (its
//       sidecars land under its own test-output dir, never the user's cache).
//       Fails if no test output directory is configured.
//
//   <wait-analysis timeoutMs="15000"/>
//       Pumps the event loop until the project's revalidator has drained its
//       job queue (analysis lane included), no SPlainWave in the project is
//       still analyzing, AND (proposal 40 M1b) no track in the project tree
//       is still bouncing (STrack::isFeelFlowBouncing()) — the bounce render
//       itself is not a queued revalidator job (it runs on its own thread),
//       so without this a script could race past a bounce still in flight.
//       Rejects on timeout. With the revalidator disabled
//       (SMARAGD_REVAL_WORKERS=0) nothing is ever pending — passes at once.
//
//   <set-render-gate clip="0,0" ready="false"/>
//       Flips the M1 render-gate readiness of the addressed SCut (same clip
//       path addressing as resize-clip/split-clip). A gated clip freezes
//       SILENT pages; flipping either way converges without restart. Fails if
//       the clip is not found or is not an SCut.
//
//   <assert-sidecar aspect="onsets" minRecords="-1" maxRecords="-1"
//                   expectExists="true"/>
//       Globs <testOutputDir>/sidecars recursively for the newest
//       *.<aspect>.*.qaf, opens it with twQafReader, and asserts the open
//       succeeds and (when given, -1 = don't check) its record count lies in
//       [minRecords, maxRecords]. Fails if expectExists and none is found.
//
//   <assert-warp-anchor clip="0,0" src="96000" warped="96000" count="2"/>
//       Resolves the SCut at clip (same addressing as set-render-gate) and
//       asserts its user warp-anchor list (proposal 28 W1/W2). When `count`
//       is given (>=0) the anchor count must match. When `src` is present an
//       anchor with that source position must EXIST; `warped` (>=0) then also
//       pins its warped value (-1 = existence only). `src` absent = count-only
//       check. Fails if the clip is missing / not an SCut / any check fails.
//
//   <feel-flow-analyze clip="0,0"/>
//   <feel-flow-analyze target="track" trackPath="0"/>
//       Proposal 40 "Feel Flow" M1 / M1b. Default target="clip": resolves
//       the SCut at clip (same addressing as set-render-gate), requires its
//       content to be an SPlainWave, and calls
//       SPlainWave::enqueueGrooveAnalysis() — the OPT-IN groove analysis job
//       (groove.res / groove.ev, never scheduled by add-sample on its own).
//       Fails if the clip is missing, not an SCut, or its content is not a
//       plain wave.
//       target="track" (M1b): resolves the STrack at trackPath (same
//       addressing as arm-track/set-track-volume) and calls
//       STrack::startFeelFlowBounce() — background-bounces the track's own
//       post-FX root component over the whole project duration
//       (sfeelflowbounce.h) and, on completion, analyzes the bounce exactly
//       as the clip path does. Fails if the track is missing or not an
//       STrack. Pair with wait-analysis before reading the sidecars either
//       way — it now also drains any in-flight track bounce. Non-undoable:
//       scheduling a background analysis is not an edit to the arrangement.
//
//   <assert-groove-aspect aspect="groove.res|groove.ev" minRecords="-1"
//                         maxRecords="-1" expectExists="true"
//                         deltaMuHighLowMs="-1" deltaMuTolMs="-1"
//                         trackPath="" stale="-1"/>
//       Proposal 40 M1 / M1b. Globs <testOutputDir>/sidecars for the newest
//       *.<aspect>.*.qaf (same newest-file convention as assert-sidecar) and
//       asserts presence and record count like assert-sidecar, PLUS aspect-
//       specific structural checks decoded via tw/sidecar/twgrooveaspect.h:
//         groove.res — every per-unit power sample and the compliance
//           scalar of every record lie in [0,1] (AC 1's payload contract;
//           unconditional, not attribute-gated). nUnits is derived from the
//           file's own geometry (recordStride/4 - 1).
//         groove.ev — records are ascending by pos (AC 1); when
//           deltaMuHighLowMs is given (>=0 or explicitly present), the
//           records are grouped by region and pooled through the SAME
//           twGroovePoolRegionStats the pendulum core itself uses (a real
//           per-event amplitude is not carried in this aspect's payload, so
//           pooling reconstructs each event at a uniform amp=1 — the bleed
//           gate this disables is measured, on this fixture set, to fire
//           zero times regardless, see twaspects.h/twgroove.h), the
//           low/high regions are picked the same way groove_test.cc's own
//           lowHighRegions helper does (closest-to-zero mu / most-positive
//           mu), and |deltaMu - deltaMuHighLowMs| must be <= deltaMuTolMs
//           (default 0.0 when the target is given but no tolerance is).
//       Fails if expectExists and none is found, or any check fails.
//       trackPath (M1b): when given, resolves the STrack at that path
//       (same addressing as feel-flow-analyze target="track") and, when
//       stale is also given (0 or 1, -1 = don't check), asserts
//       STrack::feelFlowStale() equals it. Independent of the QAF glob
//       above — a track's staleness is a property of the HOLDER, not of any
//       file on disk.
//
//   <click-feel-flow-panel trackPath="0" button="analyze|learn"/>
//       Proposal 40 "Feel Flow" M3. Drives the Track Detail "Feel Flow"
//       section's REAL button handler (SMainWindow::clickFeelFlowPanel,
//       invoking SFeelFlowPanel::onAnalyzeClicked()/onLearnClicked() by name
//       through the meta-object system) on a throwaway off-screen instance
//       bound to the addressed track -- the mutation it makes (starting a
//       bounce; submitting learn-feel-flow) is the genuine one, since both
//       act on the model/track, never on the widget instance. "analyze" is
//       non-undoable (scheduling analysis is not an edit); "learn" submits
//       learn-feel-flow and fails the SAME way that verb does (see its own
//       doc) when the track has no bounce yet or no selection is set. Fails
//       for an unknown button or track.
//
//   <assert-feel-flow-panel trackPath="0" contains="" absent=""
//                           grabPng="" grabWidth="0" grabHeight="0"/>
//       Proposal 40 "Feel Flow" M3. Builds the REAL Track Detail "Feel Flow"
//       section off screen for the addressed track (SMainWindow::
//       describeFeelFlow, the assert-track-head shape -- a single widget,
//       never shown) and matches its describe() string
//       ("state=... mode=... trained=0/1 [compliance=... units=...
//       lean=... drive=...]", SFeelFlowPanel::describe()) against `contains`/
//       `absent` (both substring checks, empty = don't check). `grabPng`
//       (into the test output dir, `grabWidth`/`grabHeight` default to the
//       panel's own sizeHint) additionally paints the same off-screen
//       section into a PNG -- coverage, not an oracle. Fails if the track
//       is missing or not an STrack, or any check fails.
//
//   <set-feel-flow-metric trackPath="0" metric="sigma"/>
//       Proposal 40 "Feel Flow" M3b. Sets the addressed track's RUNTIME
//       band-metric selection (STrack::setFeelFlowBandMetricId) -- the same
//       plain call the panel's "Band metric" combo makes. NOT an edit and
//       not undoable: a view preference, never serialized, exactly the
//       standing feel-flow-analyze has. An empty/omitted metric resets to
//       "compliance". An id the current analysis does not carry is ACCEPTED
//       (the band falls back to compliance at paint time) -- the verb fails
//       only for a missing track.
//
//   <assert-feel-flow-pose trackPath="0" frame="0" valid="-1"
//                          minAbsSum="-1" maxAbsSum="-1" contains=""
//                          grabPng="" grabWidth="0" grabHeight="0"/>
//       Proposal 40 "Feel Flow" M3e AC 6 -- THE PUPPET's gate. Computes the
//       pose the puppet dock would paint for the addressed track at `frame`,
//       through the SAME pure function it paints from (sFeelFlowPoseAt over
//       STrack::feelFlowForUi()), and asserts it.
//
//       IT MIRRORS THE PAINT PATH'S STALE RULE: a track whose analysis is
//       STALE yields the INVALID pose regardless of what data is cached,
//       exactly as SMainWindow::updateFeelFlowPuppet_ does -- staleness is
//       decided by the painter, never inside the pose function, so the gate
//       and the screen can never disagree about it.
//
//         valid      0/1, -1 = don't check. `SFeelFlowPose::valid`.
//         minAbsSum  floor on |bounceY|+|sway|+|armSwing|+|headNod|+
//         maxAbsSum  |hipShift|; -1 = don't check. ONE bound over all five
//                    components rather than ten per-component ones: what a
//                    case actually needs to separate is "the figure is
//                    MOVING" from "the figure is STILL", and any single
//                    component can legitimately be near zero at a given
//                    phase while the body as a whole is in motion.
//         contains   substring of the describe line
//                    "pose: valid=1 bounceY=... sway=... armSwing=...
//                    headNod=... hipShift=... energySum=..." (4 decimals,
//                    sFeelFlowPoseDescribe -- the ONE spelling, shared with
//                    the widget). Use it for DETERMINISM: asserting the same
//                    full line at the same frame twice pins every component
//                    at once.
//         grabPng    additionally paints the puppet for that same track and
//                    frame into a PNG in the test output dir
//                    (SMainWindow::grabFeelFlowPuppet, a parentless
//                    off-screen widget, grabWidth/grabHeight default to
//                    220x260). Coverage, not an oracle.
//       Fails if the track is missing or not an STrack, or any check fails.
//       The describe line is always qDebug()'d, which is how a case MEASURES
//       a bound before pinning it.
//
// All are transient/test-support actions: not undoable themselves.

class SSidecarRootAction : public SAction {
public:
    SSidecarRootAction() = default;

    QString name() const override { return QStringLiteral("sidecar-root"); }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;
};

class SWaitAnalysisAction : public SAction {
public:
    SWaitAnalysisAction() = default;
    explicit SWaitAnalysisAction(int timeoutMs) : timeoutMs_(timeoutMs) {}

    QString name() const override { return QStringLiteral("wait-analysis"); }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

private:
    int timeoutMs_ = 15000;
};

class SSetRenderGateAction : public SAction {
public:
    SSetRenderGateAction() = default;
    SSetRenderGateAction(const QList<int> &clipPath, bool ready)
        : clipPath_(clipPath), ready_(ready) {}

    QString name() const override { return QStringLiteral("set-render-gate"); }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

private:
    QList<int> clipPath_;
    bool ready_ = true;
};

class SAssertSidecarAction : public SAction {
public:
    SAssertSidecarAction() = default;

    QString name() const override { return QStringLiteral("assert-sidecar"); }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

private:
    QString aspect_;
    int64_t minRecords_ = -1;
    int64_t maxRecords_ = -1;
    bool expectExists_ = true;
};

class SAssertWarpAnchorAction : public SAction {
public:
    SAssertWarpAnchorAction() = default;

    QString name() const override { return QStringLiteral("assert-warp-anchor"); }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

private:
    QList<int> clipPath_;
    bool       hasSrc_ = false;   // src attribute present -> check existence/value
    int64_t    src_    = 0;       // source-domain anchor position
    int64_t    warped_ = -1;      // -1 = existence only (don't pin the value)
    int64_t    count_  = -1;      // -1 = don't check the anchor count
};

// -------------------------------------------------------- feel-flow-analyze

class SFeelFlowAnalyzeAction : public SAction {
public:
    SFeelFlowAnalyzeAction() = default;
    explicit SFeelFlowAnalyzeAction(const QList<int> &clipPath) : clipPath_(clipPath) {}

    QString name() const override { return QStringLiteral("feel-flow-analyze"); }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

private:
    QList<int> clipPath_;
    // Proposal 40 M1b: "clip" (default) or "track". target="track" reads
    // trackPath_ instead of clipPath_ and bounces STrack::startFeelFlowBounce()
    // rather than resolving a clip's SPlainWave.
    QString    target_ = QStringLiteral("clip");
    QString    trackPath_;
};

// ------------------------------------------------------ assert-groove-aspect

class SAssertGrooveAspectAction : public SAction {
public:
    SAssertGrooveAspectAction() = default;

    QString name() const override { return QStringLiteral("assert-groove-aspect"); }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

private:
    QString aspect_;
    int64_t minRecords_ = -1;
    int64_t maxRecords_ = -1;
    bool    expectExists_ = true;
    bool    hasDeltaMu_ = false;
    double  deltaMuHighLowMs_ = 0.0;
    double  deltaMuTolMs_ = 0.0;
    // Proposal 40 M1b: optional track-staleness check, independent of the
    // QAF glob above.
    QString trackPath_;             // empty = don't check staleness
    int     stale_ = -1;            // -1 = don't check, else 0/1
};

// --------------------------------------------------- click-feel-flow-panel

class SClickFeelFlowPanelAction : public SAction {
public:
    SClickFeelFlowPanelAction() = default;

    QString name() const override { return QStringLiteral("click-feel-flow-panel"); }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;
    QStringList knownAttributes() const override
    { return { QStringLiteral("trackPath"), QStringLiteral("button") }; }

private:
    QString trackPath_ = QStringLiteral("0");
    QString button_;   // "analyze" | "learn"
};

// -------------------------------------------------- assert-feel-flow-panel

class SAssertFeelFlowPanelAction : public SAction {
public:
    SAssertFeelFlowPanelAction() = default;

    QString name() const override { return QStringLiteral("assert-feel-flow-panel"); }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;
    QStringList knownAttributes() const override;

private:
    QString trackPath_ = QStringLiteral("0");
    QString contains_;
    QString absent_;
    QString grabPng_;
    int     grabWidth_  = 0;
    int     grabHeight_ = 0;
};

// ------------------------------------------------------ set-feel-flow-metric

class SSetFeelFlowMetricAction : public SAction {
public:
    SSetFeelFlowMetricAction() = default;

    QString name() const override { return QStringLiteral("set-feel-flow-metric"); }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

private:
    QString trackPath_ = QStringLiteral("0");
    QString metric_;
};

// ------------------------------------------------------ assert-feel-flow-pose

/**
 * `assert-puppet-skeleton` -- proposal 44 execution plan, C0.
 *
 * Evaluates `sFeelFlowSkeletonFor` on EXPLICIT joint scalars and asserts the
 * world angles it recovers. It deliberately reads no track, no analysis and no
 * sidecar: nothing in this repo can drive an analysis-derived pose to a chosen
 * angle (the pose is a pure function of decoded aspects, `assert-feel-flow-pose`
 * only READS it, and no fixture can be authored to land `sway` on exactly 0.5),
 * so a gate on the GEOMETRY has to supply the geometry's inputs.
 *
 * That makes it a unit gate reached through the verb, which is the same
 * discipline `tagChipRect()` established: the painter and this verb call ONE
 * function, so paint and assertion cannot drift.
 *
 * `inheritTol` is the C0 assertion proper: with `arm` and `nod` at 0, the head
 * stub, the shoulder bar and both arms must each carry the TRUNK's lean to
 * within that tolerance. On the pre-C0 build they read 0.0000 at every trunk
 * angle, because every segment above the neck was built in world coordinates.
 */
class SAssertPuppetSkeletonAction : public SAction {
public:
    SAssertPuppetSkeletonAction() = default;

    QString name() const override { return QStringLiteral("assert-puppet-skeleton"); }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;
    QStringList knownAttributes() const override;

private:
    double  sway_ = 0.0, nod_ = 0.0, arm_ = 0.0, bounce_ = 0.0, hip_ = 0.0;
    double  flex_ = 0.0, twist_ = 0.0;      // the two axes 2D could not express
    double  boxW_ = 200.0, boxH_ = 400.0;
    int     expectValid_ = 1;         // the box lays out; 0 = assert it does NOT
    double  expectTrunk_ = -1000.0;   // -1000 = don't check
    double  expectFlex_  = -1000.0;
    double  expectSwingL_ = -1000.0;
    double  expectTwist_  = -1000.0;
    double  inheritTol_  = -1.0;      // -1    = don't check
    double  tol_         = 0.01;
    /** "sagittal" | "lateral" | "" (the project default). See
     *  SFeelFlowJoints::sagittalTrunk for why the plane is a field. */
    QString plane_;
    QString contains_;
};

class SAssertFeelFlowPoseAction : public SAction {
public:
    SAssertFeelFlowPoseAction() = default;

    QString name() const override { return QStringLiteral("assert-feel-flow-pose"); }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;
    QStringList knownAttributes() const override;

private:
    QString trackPath_ = QStringLiteral("0");
    qint64  frame_     = 0;
    int     valid_     = -1;    // -1 = don't check, else 0/1
    double  minAbsSum_ = -1.0;  // -1 = don't check
    double  maxAbsSum_ = -1.0;  // -1 = don't check
    QString contains_;
    QString grabPng_;
    int     grabWidth_  = 0;
    int     grabHeight_ = 0;
};

#endif // SSIDECARTESTACTIONS_H
