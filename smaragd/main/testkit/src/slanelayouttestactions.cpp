#include "app/testkit/slanelayouttestactions.h"
#include "app/actions/sactionregistry.h"
#include "app/shell/smainwindow.h"
#include "app/shell/sapplication.h"

#include <QApplication>
#include <QDir>
#include <QDomElement>
#include <QDebug>
#include <QRegularExpression>

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
    // Horizontal zoom / pan (fix/track-list-polish m). Drives the SAME calls
    // the zoom buttons/wheel make, so it is also what a save that follows
    // will persist — unlike every other field on this action.
    if (secondWidth_ > 0.0 || scrollX_ >= 0) {
        if (!win->arrangerSetZoomPan(secondWidth_, scrollX_)) {
            qWarning() << "SSetLaneViewAction: no arranger for secondWidth/scrollX";
            return {false, nullptr};
        }
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
    if (secondWidth_ > 0.0)    elem.setAttribute("secondWidth", secondWidth_);
    if (scrollX_ >= 0)         elem.setAttribute("scrollX", (qlonglong) scrollX_);
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
    secondWidth_      = elem.attribute("secondWidth", "0").toDouble();
    scrollX_          = elem.hasAttribute("scrollX")
                          ? (qlonglong) elem.attribute("scrollX").toULongLong() : -1;
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

// --- assert-lane-view (fix/track-list-polish m) ---------------------------

SApplyResult SAssertLaneViewAction::apply(SProject * /*project*/)
{
    SMainWindow *win = mainWindow();
    if (!win) {
        qWarning() << "SAssertLaneViewAction: no main window";
        return {false, nullptr};
    }
    if (expectCollapsed_ >= 0) {
        if (trackPath_.isEmpty()) {
            qWarning() << "SAssertLaneViewAction: collapsed needs trackPath";
            return {false, nullptr};
        }
        const bool got = win->isTrackCollapsed(trackPath_);
        if (got != (expectCollapsed_ != 0)) {
            qWarning() << "assert-lane-view FAILED: trackPath" << trackPath_
                       << "collapsed=" << got << "expected"
                       << (expectCollapsed_ != 0);
            return {false, nullptr};
        }
    }
    if (expectSecondWidth_ >= 0.0 || expectScrollX_ >= 0) {
        const QString view = win->arrangerDescribeView();
        if (view.isEmpty()) {
            qWarning() << "SAssertLaneViewAction: no arranger to read"
                          " secondWidth/scrollX from";
            return {false, nullptr};
        }
        if (expectSecondWidth_ >= 0.0) {
            const QRegularExpression re(QStringLiteral("secondWidth=([-0-9.]+)"));
            const QRegularExpressionMatch m = re.match(view);
            const double got = m.hasMatch() ? m.captured(1).toDouble() : -1.0;
            if (!m.hasMatch() || qAbs(got - expectSecondWidth_) > 0.01) {
                qWarning() << "assert-lane-view FAILED: secondWidth=" << got
                           << "expected" << expectSecondWidth_ << "in" << view;
                return {false, nullptr};
            }
        }
        if (expectScrollX_ >= 0) {
            const QRegularExpression re(QStringLiteral("scrollX=([0-9]+)"));
            const QRegularExpressionMatch m = re.match(view);
            const qlonglong got = m.hasMatch()
                ? (qlonglong) m.captured(1).toULongLong() : -1;
            if (!m.hasMatch() || got != expectScrollX_) {
                qWarning() << "assert-lane-view FAILED: scrollX=" << got
                           << "expected" << expectScrollX_ << "in" << view;
                return {false, nullptr};
            }
        }
    }
    return {true, nullptr};
}

void SAssertLaneViewAction::writeXml(QDomElement &elem) const
{
    if (!trackPath_.isEmpty()) elem.setAttribute("trackPath", trackPath_);
    if (expectCollapsed_ >= 0) elem.setAttribute("collapsed", expectCollapsed_);
    if (expectSecondWidth_ >= 0.0)
        elem.setAttribute("secondWidth", expectSecondWidth_);
    if (expectScrollX_ >= 0)
        elem.setAttribute("scrollX", (qlonglong) expectScrollX_);
}

bool SAssertLaneViewAction::readXml(const QDomElement &elem, int /*version*/)
{
    trackPath_ = elem.attribute("trackPath", "");
    expectCollapsed_ = elem.hasAttribute("collapsed")
        ? elem.attribute("collapsed").toInt() : -1;
    expectSecondWidth_ = elem.hasAttribute("secondWidth")
        ? elem.attribute("secondWidth").toDouble() : -1.0;
    expectScrollX_ = elem.hasAttribute("scrollX")
        ? (qlonglong) elem.attribute("scrollX").toULongLong() : -1;
    return true;
}

static const bool s_reg_assertlaneview = (
    SActionRegistry::instance().registerType(
        QStringLiteral("assert-lane-view"),
        []{ return new SAssertLaneViewAction; }
    ), true
);

// --- assert-scroll-range (fix/track-list-polish l) -------------------------

SApplyResult SAssertScrollRangeAction::apply(SProject * /*project*/)
{
    SMainWindow *win = mainWindow();
    if (!win) {
        qWarning() << "SAssertScrollRangeAction: no main window";
        return {false, nullptr};
    }
    const QString desc = win->arrangerDescribeScrollRange();
    if (desc.isEmpty()) {
        qWarning() << "SAssertScrollRangeAction: no arranger, or no rows,"
                      " to describe";
        return {false, nullptr};
    }
    if (!expectFullyVisible_.isEmpty()) {
        const bool want = expectFullyVisible_ == QStringLiteral("true");
        const bool got = desc.contains(QStringLiteral("fullyVisible=true"));
        if (got != want) {
            qWarning() << "assert-scroll-range FAILED: expected fullyVisible="
                       << want << "got" << desc;
            return {false, nullptr};
        }
    }
    if (expectMaxAtLeast_ >= 0 || expectMaxAtMost_ >= 0) {
        const QRegularExpression re(QStringLiteral("maxScroll=(-?[0-9]+)"));
        const QRegularExpressionMatch m = re.match(desc);
        const int got = m.hasMatch() ? m.captured(1).toInt() : -1;
        if (expectMaxAtLeast_ >= 0 && got < expectMaxAtLeast_) {
            qWarning() << "assert-scroll-range FAILED: maxScroll=" << got
                       << "expected >=" << expectMaxAtLeast_ << "in" << desc;
            return {false, nullptr};
        }
        if (expectMaxAtMost_ >= 0 && got > expectMaxAtMost_) {
            qWarning() << "assert-scroll-range FAILED: maxScroll=" << got
                       << "expected <=" << expectMaxAtMost_ << "in" << desc;
            return {false, nullptr};
        }
    }
    return {true, nullptr};
}

void SAssertScrollRangeAction::writeXml(QDomElement &elem) const
{
    if (!expectFullyVisible_.isEmpty())
        elem.setAttribute("expectFullyVisible", expectFullyVisible_);
    if (expectMaxAtLeast_ >= 0)
        elem.setAttribute("expectMaxAtLeast", expectMaxAtLeast_);
    if (expectMaxAtMost_ >= 0)
        elem.setAttribute("expectMaxAtMost", expectMaxAtMost_);
}

bool SAssertScrollRangeAction::readXml(const QDomElement &elem, int /*version*/)
{
    expectFullyVisible_ = elem.attribute("expectFullyVisible", "");
    expectMaxAtLeast_ = elem.hasAttribute("expectMaxAtLeast")
        ? elem.attribute("expectMaxAtLeast").toInt() : -1;
    expectMaxAtMost_ = elem.hasAttribute("expectMaxAtMost")
        ? elem.attribute("expectMaxAtMost").toInt() : -1;
    return true;
}

static const bool s_reg_assertscrollrange = (
    SActionRegistry::instance().registerType(
        QStringLiteral("assert-scroll-range"),
        []{ return new SAssertScrollRangeAction; }
    ), true
);
