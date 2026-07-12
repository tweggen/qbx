#include "app/objects/track/ssettrackvolumeaction.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/actions/sactionregistry.h"
#include "app/objects/track/strack.h"
#include "app/model/slink.h"
#include <QDomElement>

SSetTrackVolumeAction::SSetTrackVolumeAction(int trackIdx, double newVolume)
    : trackIndex_(trackIdx), newVolume_(newVolume)
{
}

SApplyResult SSetTrackVolumeAction::apply(SProject *project)
{
    if (!project) {
        return {false, nullptr};
    }

    SObject *root = splacements::rootContainer( project );
    SObject *mixer = root;
    if (!mixer) {
        return {false, nullptr};
    }

    // Resolve the track at the stored index.
    SLink *trackLink = mixer->childAt(trackIndex_);
    if (!trackLink) {
        return {false, nullptr};
    }

    SObject *trackObj = &trackLink->getSObject();
    STrack *track = dynamic_cast<STrack*>(trackObj);
    if (!track) {
        return {false, nullptr};
    }

    // Capture pre-mutation state for the inverse, then mutate.
    // setVolume() emits volumeChanged(), which propagates to the audio mixer
    // (SStdMixer::trackVolumeChanged -> setInputLevel) and the UI spinbox.
    double oldVolume = track->getVolume();
    track->setVolume(newVolume_);

    SSetTrackVolumeAction *inverse = new SSetTrackVolumeAction(trackIndex_, oldVolume);
    return {true, inverse};
}

QString SSetTrackVolumeAction::mergeKey() const
{
    // Same track -> same key, so consecutive drags coalesce.
    return QStringLiteral("set-track-volume:%1").arg(trackIndex_);
}

bool SSetTrackVolumeAction::mergeWith(const SAction *later)
{
    const SSetTrackVolumeAction *o = dynamic_cast<const SSetTrackVolumeAction*>(later);
    if (!o || o->trackIndex_ != trackIndex_) {
        return false;
    }
    // Absorb the newer target volume; keep our own (older) value as the
    // baseline the inverse was captured against.
    newVolume_ = o->newVolume_;
    return true;
}

void SSetTrackVolumeAction::writeXml(QDomElement &elem) const
{
    elem.setAttribute("trackIndex", trackIndex_);
    elem.setAttribute("volume", QString::number(newVolume_));
}

bool SSetTrackVolumeAction::readXml(const QDomElement &elem, int /*version*/)
{
    trackIndex_ = elem.attribute("trackIndex", "0").toInt();
    newVolume_ = elem.attribute("volume", "0").toDouble();
    return true;
}

static const bool s_reg_settrackvolume = (
    SActionRegistry::instance().registerType(
        QStringLiteral("set-track-volume"),
        []{ return new SSetTrackVolumeAction; }
    ), true
);
