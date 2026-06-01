#include "actions/stoggleplaybackaction.h"
#include "sproject.h"
#include "sapplication.h"
#include "twspeaker.h"
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

    // Not undoable (transport control).
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
