#include "app/testkit/slanelayouttestactions.h"
#include "app/actions/sactionregistry.h"
#include "app/shell/smainwindow.h"
#include <QApplication>
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
}

bool SSetLaneViewAction::readXml(const QDomElement &elem, int /*version*/)
{
    laneScaleRow_    = elem.attribute("laneScaleRow", "-1").toInt();
    laneScale_       = elem.attribute("laneScale", "0").toDouble();
    toggleTakesRow_  = elem.attribute("toggleTakesRow", "-1").toInt();
    baseTrackHeight_ = elem.attribute("trackHeight", "0").toInt();
    topRow_          = elem.attribute("topRow", "-1").toInt();
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
    return {true, nullptr};
}

void SAssertLaneAlignmentAction::writeXml(QDomElement & /*elem*/) const
{
}

bool SAssertLaneAlignmentAction::readXml(const QDomElement & /*elem*/, int /*version*/)
{
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
