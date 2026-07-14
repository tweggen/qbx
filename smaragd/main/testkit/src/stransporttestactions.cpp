#include "app/testkit/stransporttestactions.h"
#include "app/actions/sactionregistry.h"
#include "app/shell/sapplication.h"
#include "app/model/sproject.h"
#include "app/model/slink.h"
#include "app/objects/mixer/sstdmixer.h"
#include "app/objects/track/strack.h"
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

// ------------------------------------------------------------- set-track-mute

SApplyResult SSetTrackMuteAction::apply(SProject *project)
{
    if (!project) {
        return {false, nullptr};
    }

    SStdMixer *mixer = dynamic_cast<SStdMixer*>(project->getRootComponent());
    if (!mixer || trackIndex_ < 0 || trackIndex_ >= mixer->getNTracks()) {
        qWarning() << "set-track-mute: no track at index" << trackIndex_;
        return {false, nullptr};
    }

    SLink *link = mixer->getTrackAt(trackIndex_);
    if (!link) {
        return {false, nullptr};
    }

    STrack *track = dynamic_cast<STrack*>(&link->getSObject());
    if (!track) {
        qWarning() << "set-track-mute: child" << trackIndex_ << "is not a track";
        return {false, nullptr};
    }

    track->setMuted(muted_);
    return {true, nullptr};
}

void SSetTrackMuteAction::writeXml(QDomElement &elem) const
{
    elem.setAttribute("trackIndex", QString::number(trackIndex_));
    elem.setAttribute("muted", muted_ ? "1" : "0");
}

bool SSetTrackMuteAction::readXml(const QDomElement &elem, int /*version*/)
{
    trackIndex_ = elem.attribute("trackIndex", "0").toInt();
    muted_ = elem.attribute("muted", "0") == "1";
    return true;
}

static const bool s_reg_set_track_mute = (
    SActionRegistry::instance().registerType(
        QStringLiteral("set-track-mute"),
        []{ return new SSetTrackMuteAction; }
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
