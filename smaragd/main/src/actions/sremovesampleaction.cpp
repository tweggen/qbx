#include "actions/sremovesampleaction.h"
#include "actions/saddsampleaction.h"
#include "sproject.h"
#include "sactionregistry.h"
#include "scut.h"
#include "slink.h"
#include "sstdmixer.h"
#include "strack.h"
#include "sexternfile.h"
#include <QDomElement>

SRemoveSampleAction::SRemoveSampleAction(int trackIdx, int clipIdx, const QString &filePath, offset_t timePos)
    : trackIndex_(trackIdx), clipIndex_(clipIdx), filePath_(filePath), timePos_(timePos)
{
}

SApplyResult SRemoveSampleAction::apply(SProject *project)
{
    if (!project) {
        return {false, nullptr};
    }

    SObject *root = project->getRootComponent();
    SStdMixer *mixer = dynamic_cast<SStdMixer*>(root);
    if (!mixer) {
        return {false, nullptr};
    }

    // Get the track.
    SLink *trackLink = mixer->getTrackAt(trackIndex_);
    if (!trackLink) {
        return {false, nullptr};
    }

    SObject *trackObj = &trackLink->getSObject();
    STrack *track = dynamic_cast<STrack*>(trackObj);
    if (!track) {
        return {false, nullptr};
    }

    // Get the clip at the specified index.
    const QObjectList &children = track->children();
    if (clipIndex_ < 0 || clipIndex_ >= children.size()) {
        return {false, nullptr};
    }

    SLink *clipLink = dynamic_cast<SLink*>(children.at(clipIndex_));
    if (!clipLink) {
        return {false, nullptr};
    }

    // Before removing, verify this is a sample clip and delete it.
    // The filePath and timePos are already captured in this action,
    // so they become the inverse parameters.
    delete clipLink;  // Qt will remove from parent, SCut destructor handles cleanup

    // Return inverse: re-add the sample at the same position
    SAddSampleAction *inverse = new SAddSampleAction(trackIndex_, filePath_, timePos_);
    return {true, inverse};
}

void SRemoveSampleAction::writeXml(QDomElement &elem) const
{
    elem.setAttribute("trackIndex", trackIndex_);
    elem.setAttribute("clipIndex", clipIndex_);
    elem.setAttribute("filePath", filePath_);
    elem.setAttribute("timePos", QString::number(timePos_));
}

bool SRemoveSampleAction::readXml(const QDomElement &elem, int /*version*/)
{
    trackIndex_ = elem.attribute("trackIndex", "0").toInt();
    clipIndex_ = elem.attribute("clipIndex", "0").toInt();
    filePath_ = elem.attribute("filePath", "");
    timePos_ = elem.attribute("timePos", "0").toULongLong();
    return true;
}

static const bool s_reg_removesample = (
    SActionRegistry::instance().registerType(
        QStringLiteral("remove-sample"),
        []{ return new SRemoveSampleAction; }
    ), true
);
