#include "app/testkit/stransporttestactions.h"
#include "app/actions/sactionregistry.h"
#include "app/shell/sapplication.h"
#include "app/shell/smainwindow.h"
#include "app/actions/sactionhistory.h"
#include "app/model/sproject.h"
#include "app/model/slink.h"
#include "app/objects/mixer/sstdmixer.h"
#include "app/objects/track/strack.h"
#include "app/model/sobjectpath.h"
#include <QApplication>
#include <QCoreApplication>
#include <QDomElement>
#include <QElapsedTimer>
#include <QThread>
#include <QDebug>

// ---------------------------------------------------------------- set-locator

SApplyResult SSetLocatorAction::apply(SProject *project)
{
    if (!project) {
        return {false, nullptr};
    }
    // The scripted stand-in for a user clicking the ruler/timeline to seek —
    // a direct navigation (item o), so it updates lastNavigatedLocatorPos_
    // exactly as sstdmixerview.cpp's real click handler does.
    SApplication::app().setGlobalLocatorPos((offset_t)position_);
    SApplication::app().noteUserNavigatedLocator((offset_t)position_);
    return {true, nullptr};
}

void SSetLocatorAction::writeXml(QDomElement &elem) const
{
    elem.setAttribute("position", QString::number(position_));
}

bool SSetLocatorAction::readXml(const QDomElement &elem, int /*version*/)
{
    position_ = elem.attribute("position", "0").toULongLong();
    return true;
}

static const bool s_reg_set_locator = (
    SActionRegistry::instance().registerType(
        QStringLiteral("set-locator"),
        []{ return new SSetLocatorAction; }
    ), true
);

// set-track-mute used to live here. It is a real, undoable edit now (the mute
// button submits it), so it moved to
// main/objects/track/src/ssettrackmuteaction.cpp next to set-track-solo. Its
// XML is unchanged, index form included, so the committed cases are untouched.

// -------------------------------------------------------------- assert-locator

SApplyResult SAssertLocatorAction::apply(SProject *project)
{
    if (!project) {
        return {false, nullptr};
    }
    const offset_t actual = SApplication::app().getGlobalLocatorPos();
    if ((offset_t)position_ != actual) {
        qWarning() << "assert-locator FAILED: expected" << (qulonglong)position_
                   << "but the locator is at" << (qulonglong)actual;
        return {false, nullptr};
    }
    return {true, nullptr};
}

void SAssertLocatorAction::writeXml(QDomElement &elem) const
{
    elem.setAttribute("position", QString::number(position_));
}

bool SAssertLocatorAction::readXml(const QDomElement &elem, int /*version*/)
{
    position_ = elem.attribute("position", "0").toULongLong();
    return true;
}

static const bool s_reg_assert_locator = (
    SActionRegistry::instance().registerType(
        QStringLiteral("assert-locator"),
        []{ return new SAssertLocatorAction; }
    ), true
);

// -------------------------------------------------------------- wait-playhead

SApplyResult SWaitPlayheadAction::apply(SProject *project)
{
    if (!project) {
        return {false, nullptr};
    }

    SApplication &app = SApplication::app();
    const offset_t start = app.getGlobalLocatorPos();

    QElapsedTimer timer;
    timer.start();

    // Two independent conditions, both optional, both must hold:
    //   minAdvance — RELATIVE: "playback moved". Survives not knowing where
    //                playback started.
    //   position   — ABSOLUTE: "playback arrived HERE". The one a positional
    //                assertion needs, since it is the position, not the
    //                distance, that the following decode is about.
    // The relative measurement re-anchors on a backwards jump (loop wrap, a
    // seek); the absolute one deliberately does not — a wrap past the target
    // means the target was reached.
    offset_t current = start;
    offset_t anchor = start;
    bool reachedPosition = (position_ == 0);
    for (;;) {
        const bool advanced = (qulonglong)(current - anchor) >= minAdvance_;
        if (advanced && reachedPosition) {
            break;
        }
        if (timer.elapsed() > timeoutMs_) {
            qWarning() << "wait-playhead: TIMEOUT after" << timeoutMs_ << "ms;"
                       << "playhead advanced" << (qulonglong)(current - anchor)
                       << "of" << minAdvance_ << "frames (start"
                       << (qulonglong)start << "now" << (qulonglong)current << ")"
                       << (position_ ? QString("; target position %1 %2 reached")
                                           .arg(position_)
                                           .arg(reachedPosition ? "was" : "NOT")
                                     : QString());
            return {false, nullptr};
        }
        QCoreApplication::processEvents();
        QThread::msleep(10);
        current = app.getGlobalLocatorPos();
        if (position_ != 0 && current >= (offset_t)position_) {
            reachedPosition = true;
        }
        if (current < anchor) {
            // Locator jumped backwards (loop wrap or user seek): re-anchor so
            // the advance measurement stays meaningful.
            anchor = current;
        }
    }

    qDebug() << "wait-playhead: playhead advanced" << (qulonglong)(current - anchor)
             << "frames in" << timer.elapsed() << "ms, now at"
             << (qulonglong)current << "(OK)";
    return {true, nullptr};
}

void SWaitPlayheadAction::writeXml(QDomElement &elem) const
{
    elem.setAttribute("minAdvance", QString::number(minAdvance_));
    elem.setAttribute("timeoutMs", QString::number(timeoutMs_));
    if (position_ != 0) {
        elem.setAttribute("position", QString::number(position_));
    }
}

bool SWaitPlayheadAction::readXml(const QDomElement &elem, int /*version*/)
{
    minAdvance_ = elem.attribute("minAdvance", "0").toULongLong();
    position_ = elem.attribute("position", "0").toULongLong();
    timeoutMs_ = elem.attribute("timeoutMs", "10000").toInt();
    return true;
}

static const bool s_reg_wait_playhead = (
    SActionRegistry::instance().registerType(
        QStringLiteral("wait-playhead"),
        []{ return new SWaitPlayheadAction; }
    ), true
);

// -------------------------------------------------------------- undo / redo

// These drive the REAL undo stack rather than re-applying a hand-built inverse,
// so a case exercises exactly what Ctrl-Z does in the GUI — including an
// inverse that refuses to apply (SApplyResult.applied == false), which used to
// look like a no-op from the outside.
SApplyResult SUndoAction::apply(SProject * /*project*/)
{
    SActionHistory *history = SApplication::app().actionHistory();
    if (!history) {
        return {false, nullptr};
    }
    for (int i = 0; i < count_; ++i) {
        history->undo();
    }
    return {true, nullptr};
}

void SUndoAction::writeXml(QDomElement &elem) const
{
    elem.setAttribute("count", QString::number(count_));
}

bool SUndoAction::readXml(const QDomElement &elem, int /*version*/)
{
    count_ = elem.attribute("count", "1").toInt();
    if (count_ < 1) count_ = 1;
    return true;
}

static const bool s_reg_undo = (
    SActionRegistry::instance().registerType(
        QStringLiteral("undo"),
        []{ return new SUndoAction; }
    ), true
);

SApplyResult SRedoAction::apply(SProject * /*project*/)
{
    SActionHistory *history = SApplication::app().actionHistory();
    if (!history) {
        return {false, nullptr};
    }
    for (int i = 0; i < count_; ++i) {
        history->redo();
    }
    return {true, nullptr};
}

void SRedoAction::writeXml(QDomElement &elem) const
{
    elem.setAttribute("count", QString::number(count_));
}

bool SRedoAction::readXml(const QDomElement &elem, int /*version*/)
{
    count_ = elem.attribute("count", "1").toInt();
    if (count_ < 1) count_ = 1;
    return true;
}

static const bool s_reg_redo = (
    SActionRegistry::instance().registerType(
        QStringLiteral("redo"),
        []{ return new SRedoAction; }
    ), true
);

// -------------------------------------------------------------- press-play

// Drives the REAL Space / Shift+Space handlers (item o), not a re-spelling of
// them, exactly as `undo`/`redo` above drive the real undo stack — see the
// header comment for why `toggle-playback` cannot stand in for this.
SApplyResult SPressPlayAction::apply(SProject * /*project*/)
{
    SMainWindow *win = nullptr;
    for (QWidget *w : QApplication::topLevelWidgets()) {
        if ((win = qobject_cast<SMainWindow *>(w))) break;
    }
    if (!win) {
        qWarning() << "press-play: no main window";
        return {false, nullptr};
    }
    if (shift_) win->startPlayingFromCurrent();
    else        win->startPlaying();
    // Not undoable: transport state is transient, like toggle-playback.
    return {true, nullptr};
}

void SPressPlayAction::writeXml(QDomElement &elem) const
{
    elem.setAttribute("shift", shift_ ? "1" : "0");
}

bool SPressPlayAction::readXml(const QDomElement &elem, int /*version*/)
{
    const QString s = elem.attribute("shift", "0");
    shift_ = (s == "1" || s == "true");
    return true;
}

static const bool s_reg_pressplay = (
    SActionRegistry::instance().registerType(
        QStringLiteral("press-play"),
        []{ return new SPressPlayAction; }
    ), true
);

// -------------------------------------------------------------- press-stop

// Drives the REAL Stop button/shortcut — see the header comment for why it
// matters that this is stopPlaying() itself and not toggle-playback.
SApplyResult SPressStopAction::apply(SProject * /*project*/)
{
    SMainWindow *win = nullptr;
    for (QWidget *w : QApplication::topLevelWidgets()) {
        if ((win = qobject_cast<SMainWindow *>(w))) break;
    }
    if (!win) {
        qWarning() << "press-stop: no main window";
        return {false, nullptr};
    }
    win->stopPlaying();
    // Not undoable: transport state is transient, like toggle-playback.
    return {true, nullptr};
}

void SPressStopAction::writeXml(QDomElement & /*elem*/) const
{
}

bool SPressStopAction::readXml(const QDomElement & /*elem*/, int /*version*/)
{
    return true;
}

static const bool s_reg_pressstop = (
    SActionRegistry::instance().registerType(
        QStringLiteral("press-stop"),
        []{ return new SPressStopAction; }
    ), true
);
