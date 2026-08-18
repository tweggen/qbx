#include "app/testkit/slanelayouttestactions.h"
#include "app/actions/sactionregistry.h"
#include "app/shell/smainwindow.h"
#include "app/shell/sapplication.h"

#include <QApplication>
#include <QDir>
#include <QDomElement>
#include <QDebug>

// The arranger lives under the main window, and testkit may not include
// app/timeline (tools/check_layering.py) — so both actions reach it through
// the shell, the same route drag-clip-edge uses.
static SMainWindow *mainWindow()
{
    for (QWidget *w : QApplication::topLevelWidgets()) {
        if (SMainWindow *win = qobject_cast<SMainWindow*>(w)) return win;
    }
    return nullptr;
}

SApplyResult SSetLaneViewAction::apply(SProject * /*project*/)
{
    SMainWindow *win = mainWindow();
    if (!win) {
        qWarning() << "SSetLaneViewAction: no main window";
        return {false, nullptr};
    }
    if (!win->arrangerSetLaneView(laneScaleRow_, laneScale_, toggleTakesRow_,
                                  baseTrackHeight_, topRow_)) {
        qWarning() << "SSetLaneViewAction: could not apply the requested view state";
        return {false, nullptr};
    }
    // Automation lane visibility (proposal 37 P6) — the same per-track,
    // never-saved kind of view state.
    if (showAutomation_ >= 0) {
        if (automationTarget_.isEmpty()) {
            qWarning() << "SSetLaneViewAction: showAutomation needs automationTarget";
            return {false, nullptr};
        }
        if (!win->showAutomationLane(automationTrack_, automationTarget_,
                                     automationSlot_, showAutomation_ != 0)) {
            qWarning() << "SSetLaneViewAction: no track at"
                       << automationTrack_ << "for" << automationTarget_;
            return {false, nullptr};
        }
    }
    if (clipEnvelopes_ >= 0 && !win->setClipEnvelopeEdit(clipEnvelopes_ != 0)) {
        qWarning() << "SSetLaneViewAction: no arranger for clipEnvelopes";
        return {false, nullptr};
    }
    return {true, nullptr};   // view state: nothing to undo
}

void SSetLaneViewAction::writeXml(QDomElement &elem) const
{
    if (laneScaleRow_ >= 0) {
        elem.setAttribute("laneScaleRow", laneScaleRow_);
        elem.setAttribute("laneScale", laneScale_);
    }
    if (toggleTakesRow_ >= 0)  elem.setAttribute("toggleTakesRow", toggleTakesRow_);
    if (baseTrackHeight_ > 0)  elem.setAttribute("trackHeight", baseTrackHeight_);
    if (topRow_ >= 0)          elem.setAttribute("topRow", topRow_);
    if (showAutomation_ >= 0) {
        elem.setAttribute("automationTrack", automationTrack_);
        elem.setAttribute("automationTarget", automationTarget_);
        elem.setAttribute("showAutomation", showAutomation_);
        if (automationSlot_ >= 0) elem.setAttribute("automationSlot", automationSlot_);
    }
    if (clipEnvelopes_ >= 0)   elem.setAttribute("clipEnvelopes", clipEnvelopes_);
}

bool SSetLaneViewAction::readXml(const QDomElement &elem, int /*version*/)
{
    laneScaleRow_    = elem.attribute("laneScaleRow", "-1").toInt();
    laneScale_       = elem.attribute("laneScale", "0").toDouble();
    toggleTakesRow_  = elem.attribute("toggleTakesRow", "-1").toInt();
    baseTrackHeight_ = elem.attribute("trackHeight", "0").toInt();
    topRow_          = elem.attribute("topRow", "-1").toInt();
    automationTrack_  = elem.attribute("automationTrack", "0");
    automationTarget_ = elem.attribute("automationTarget", "");
    automationSlot_   = elem.attribute("automationSlot", "-1").toInt();
    showAutomation_   = elem.hasAttribute("showAutomation")
                          ? elem.attribute("showAutomation").toInt() : -1;
    clipEnvelopes_    = elem.hasAttribute("clipEnvelopes")
                          ? elem.attribute("clipEnvelopes").toInt() : -1;
    if (showAutomation_ >= 0 && automationTarget_.isEmpty()) {
        qWarning() << "SSetLaneViewAction: showAutomation needs automationTarget";
        return false;
    }
    if (laneScaleRow_ >= 0 && laneScale_ <= 0.0) {
        qWarning() << "SSetLaneViewAction::readXml: laneScaleRow needs a positive laneScale";
        return false;
    }
    return true;
}

SApplyResult SAssertLaneAlignmentAction::apply(SProject * /*project*/)
{
    SMainWindow *win = mainWindow();
    if (!win) {
        qWarning() << "SAssertLaneAlignmentAction: no main window";
        return {false, nullptr};
    }
    const QString problem = win->arrangerLaneAlignment();
    if (!problem.isEmpty()) {
        qWarning() << "assert-lane-alignment FAILED:" << problem;
        return {false, nullptr};
    }

    // The PNG is coverage, never an oracle (design 6.4): it is the only thing
    // that proves drawAutomationLane paints at all.
    if (!grabPng_.isEmpty()) {
        if (grabPng_.contains('/') || grabPng_.contains('\\')
            || grabPng_.contains("..")) {
            qWarning() << "assert-lane-alignment: grabPng contains path"
                          " separators:" << grabPng_;
            return {false, nullptr};
        }
        SApplication &app = SApplication::app();
        if (app.testOutputDir().isEmpty() || !app.ensureOutputDirExists()) {
            qWarning() << "assert-lane-alignment FAILED: no usable test output"
                          " directory";
            return {false, nullptr};
        }
        const QString out = QDir(app.testOutputDir()).filePath(grabPng_);
        if (!win->grabArrangerLanes(out, grabWidth_, grabHeight_)) {
            qWarning() << "assert-lane-alignment FAILED: could not grab the"
                          " arranger into" << out;
            return {false, nullptr};
        }
    }
    return {true, nullptr};
}

void SAssertLaneAlignmentAction::writeXml(QDomElement &elem) const
{
    if (!grabPng_.isEmpty()) {
        elem.setAttribute("grabPng", grabPng_);
        if (grabWidth_ > 0)  elem.setAttribute("grabWidth", grabWidth_);
        if (grabHeight_ > 0) elem.setAttribute("grabHeight", grabHeight_);
    }
}

bool SAssertLaneAlignmentAction::readXml(const QDomElement &elem, int /*version*/)
{
    grabPng_    = elem.attribute("grabPng", "");
    grabWidth_  = elem.attribute("grabWidth", "0").toInt();
    grabHeight_ = elem.attribute("grabHeight", "0").toInt();
    return true;
}

static const bool s_reg_setlaneview = (
    SActionRegistry::instance().registerType(
        QStringLiteral("set-lane-view"),
        []{ return new SSetLaneViewAction; }
    ), true
);

static const bool s_reg_assertlanealignment = (
    SActionRegistry::instance().registerType(
        QStringLiteral("assert-lane-alignment"),
        []{ return new SAssertLaneAlignmentAction; }
    ), true
);

// --- collapse-track (proposal 39 M3a) -------------------------------------

SApplyResult SCollapseTrackAction::apply(SProject * /*project*/)
{
    SMainWindow *win = mainWindow();
    if (!win) {
        qWarning() << "SCollapseTrackAction: no main window";
        return {false, nullptr};
    }
    if (!win->setTrackCollapsed(trackPath_, collapsed_)) {
        qWarning() << "SCollapseTrackAction: no lane at" << trackPath_;
        return {false, nullptr};
    }
    return {true, nullptr};   // view state: nothing to undo
}

void SCollapseTrackAction::writeXml(QDomElement &elem) const
{
    elem.setAttribute("trackPath", trackPath_);
    elem.setAttribute("collapsed", collapsed_ ? 1 : 0);
}

bool SCollapseTrackAction::readXml(const QDomElement &elem, int /*version*/)
{
    trackPath_ = elem.attribute("trackPath", "0");
    collapsed_ = elem.attribute("collapsed", "1").toInt() != 0;
    return true;
}

static const bool s_reg_collapsetrack = (
    SActionRegistry::instance().registerType(
        QStringLiteral("collapse-track"),
        []{ return new SCollapseTrackAction; }
    ), true
);
