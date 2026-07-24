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
//       job queue (analysis lane included) AND no SPlainWave in the project is
//       still analyzing. Rejects on timeout. With the revalidator disabled
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

#endif // SSIDECARTESTACTIONS_H
