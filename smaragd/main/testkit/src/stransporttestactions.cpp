#include "app/testkit/stransporttestactions.h"
#include "app/actions/sactionregistry.h"
#include "app/shell/sapplication.h"
#include "app/actions/sactionhistory.h"
#include "app/model/sproject.h"
#include "app/model/slink.h"
#include "app/objects/mixer/sstdmixer.h"
#include "app/objects/track/strack.h"
#include "app/model/sobjectpath.h"
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
    SApplication::app().setGlobalLocatorPos((offset_t)position_);
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

    offset_t current = start;
    while ((qulonglong)(current - start) < minAdvance_) {
        if (timer.elapsed() > timeoutMs_) {
            qWarning() << "wait-playhead: TIMEOUT after" << timeoutMs_ << "ms;"
                       << "playhead advanced" << (qulonglong)(current - start)
                       << "of" << minAdvance_ << "frames (start" << (qulonglong)start
                       << "now" << (qulonglong)current << ")";
            return {false, nullptr};
        }
        QCoreApplication::processEvents();
        QThread::msleep(10);
        current = app.getGlobalLocatorPos();
        if (current < start) {
            // Locator jumped backwards (loop wrap or user seek): re-anchor so
            // the advance measurement stays meaningful.
            current = start;
        }
    }

    qDebug() << "wait-playhead: playhead advanced" << (qulonglong)(current - start)
             << "frames in" << timer.elapsed() << "ms (OK)";
    return {true, nullptr};
}

void SWaitPlayheadAction::writeXml(QDomElement &elem) const
{
    elem.setAttribute("minAdvance", QString::number(minAdvance_));
    elem.setAttribute("timeoutMs", QString::number(timeoutMs_));
}

bool SWaitPlayheadAction::readXml(const QDomElement &elem, int /*version*/)
{
    minAdvance_ = elem.attribute("minAdvance", "0").toULongLong();
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
