#include "app/actions/stoggleplaybackaction.h"
#include "app/model/sproject.h"
#include "app/model/sappcontext.h"
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

    SAppContext::get().setPlaybackRunning( play_ );

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
