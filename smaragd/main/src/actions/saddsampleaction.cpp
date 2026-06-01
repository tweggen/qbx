#include "actions/saddsampleaction.h"
#include "sproject.h"
#include "sstdmixer.h"
#include "strack.h"
#include "scut.h"
#include "slink.h"
#include <QDomElement>

SAddSampleAction::SAddSampleAction(int trackIdx, const QString &filePath, offset_t timePos)
    : trackIndex_(trackIdx), filePath_(filePath), timePos_(timePos)
{
}

SApplyResult SAddSampleAction::apply(SProject *project)
{
    if (!project) {
        return {false, nullptr};
    }

    SObject *root = project->getRootComponent();
    SStdMixer *mixer = dynamic_cast<SStdMixer*>(root);
    if (!mixer) {
        return {false, nullptr};
    }

    // Get the track at the specified index.
    SLink *trackLink = mixer->getTrackAt(trackIndex_);
    if (!trackLink) {
        return {false, nullptr};
    }

    SObject *trackObj = &trackLink->getSObject();
    STrack *track = dynamic_cast<STrack*>(trackObj);
    if (!track) {
        return {false, nullptr};
    }

    // Link to the file.
    QString mutablePath = filePath_;
    SLink *wavLink = project->linkToFile(mutablePath);
    if (!wavLink) {
        return {false, nullptr};
    }

    // Create a cut from the WAV link.
    SCut *cut = new SCut(project, *wavLink);
    if (!cut) {
        return {false, nullptr};
    }

    // Create a new link from the cut, with track as parent.
    SLink *cutLink = new SLink(*cut, track);
    if (!cutLink) {
        return {false, nullptr};
    }

    // Set the start time.
    cutLink->setStartTime(timePos_);

    // Phase 1: undo not implemented.
    return {true, nullptr};
}

void SAddSampleAction::writeXml(QDomElement &elem) const
{
    elem.setAttribute("trackIndex", trackIndex_);
    elem.setAttribute("filePath", filePath_);
    elem.setAttribute("timePos", QString::number(timePos_));
}

bool SAddSampleAction::readXml(const QDomElement &elem, int /*version*/)
{
    trackIndex_ = elem.attribute("trackIndex", "0").toInt();
    filePath_ = elem.attribute("filePath", "");
    timePos_ = elem.attribute("timePos", "0").toULongLong();
    return true;
}
