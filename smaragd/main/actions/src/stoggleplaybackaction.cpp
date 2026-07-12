#include "app/actions/stoggleplaybackaction.h"
#include "app/model/sproject.h"
#include "app/shell/sapplication.h"
#include "tw/playback/twspeaker.h"
#include "app/actions/sactionregistry.h"
#include <QDomElement>

STogglePlaybackAction::STogglePlaybackAction(bool play)
    : play_(play)
{
}

SApplyResult STogglePlaybackAction::apply(SProject *project)
{
    if (!project) {
        return {false, nullptr};
    }

    SApplication &app = SApplication::app();
    twSpeaker *speaker = app.getSpeaker();
    if (!speaker) {
        return {false, nullptr};
    }

    if (play_) {
        speaker->startOutput();
        app.setPlaying(true);
    } else {
        speaker->stopOutput();
        app.setPlaying(false);
    }

    // Not undoable: playback state is transient and not persisted in the project.
    return {true, nullptr};
}

void STogglePlaybackAction::writeXml(QDomElement &elem) const
{
    elem.setAttribute("play", play_ ? "1" : "0");
}

bool STogglePlaybackAction::readXml(const QDomElement &elem, int /*version*/)
{
    play_ = elem.attribute("play", "0") == "1";
    return true;
}

static const bool s_reg_toggleplayback = (
    SActionRegistry::instance().registerType(
        QStringLiteral("toggle-playback"),
        []{ return new STogglePlaybackAction(false); }
    ), true
);
