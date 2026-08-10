#include "app/objects/track/ssettrackvolumeaction.h"
#include "app/model/sobjectpath.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/actions/sactionregistry.h"
#include "app/objects/track/strack.h"
#include "app/model/slink.h"
#include <QDomElement>

using namespace strackpath;

SSetTrackVolumeAction::SSetTrackVolumeAction(const QList<int> &trackPath, double newVolume)
    : trackPath_(trackPath), newVolume_(newVolume)
{
}

SApplyResult SSetTrackVolumeAction::apply(SProject *project)
{
    SObject *mixer = splacements::rootContainer( project );
    // Path-addressed, so a track nested inside a folder resolves as readily as
    // a top-level one.
    SObject *lane = splacements::laneAt( mixer, trackPath_ );
    if (!lane) {
        return {false, nullptr};
    }

    // Volume is an STrack property (the root mixer is a lane but has no fader).
    STrack *track = dynamic_cast<STrack*>(lane);
    if (!track) {
        return {false, nullptr};
    }

    // Capture pre-mutation state for the inverse, then mutate.
    // setVolume() emits volumeChanged(), which propagates to the audio mixer
    // (SStdMixer::trackVolumeChanged -> setInputLevel) and the UI spinbox.
    double oldVolume = track->getVolume();
    track->setVolume(newVolume_);

    SSetTrackVolumeAction *inverse = new SSetTrackVolumeAction(trackPath_, oldVolume);
    return {true, inverse};
}

QString SSetTrackVolumeAction::mergeKey() const
{
    // Same lane -> same key, so consecutive drags coalesce. Keyed on the PATH:
    // a bare index would collide across folders (top-level track 0 and the
    // first child of a folder both being "0"), merging two different faders'
    // drags into one undo step.
    return QStringLiteral("set-track-volume:%1").arg(pathToString(trackPath_));
}

bool SSetTrackVolumeAction::mergeWith(const SAction *later)
{
    const SSetTrackVolumeAction *o = dynamic_cast<const SSetTrackVolumeAction*>(later);
    if (!o || o->trackPath_ != trackPath_) {
        return false;
    }
    // Absorb the newer target volume; keep our own (older) value as the
    // baseline the inverse was captured against.
    newVolume_ = o->newVolume_;
    return true;
}

void SSetTrackVolumeAction::writeXml(QDomElement &elem) const
{
    elem.setAttribute("trackPath", pathToString(trackPath_));
    elem.setAttribute("volume", QString::number(newVolume_));
}

bool SSetTrackVolumeAction::readXml(const QDomElement &elem, int /*version*/)
{
    // Sniff the spelling rather than key off formatVersion(): pre-existing .qxa
    // scripts carry no version attribute, and `trackIndex` is exactly a
    // one-element path.
    trackPath_ = elem.hasAttribute("trackPath")
        ? stringToPath( elem.attribute("trackPath") )
        : QList<int>{ elem.attribute("trackIndex", "0").toInt() };
    newVolume_ = elem.attribute("volume", "0").toDouble();
    return true;
}

static const bool s_reg_settrackvolume = (
    SActionRegistry::instance().registerType(
        QStringLiteral("set-track-volume"),
        []{ return new SSetTrackVolumeAction; }
    ), true
);
