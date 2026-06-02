#include "actions/saddsampleaction.h"
#include "sproject.h"
#include "sactionregistry.h"
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

    // Create a new link from the cut. IMPORTANT: construct with NULL parent,
    // then setParent after construction to avoid triggering childEvent during init.
    SLink *cutLink = new SLink(*cut, NULL);
    if (!cutLink) {
        return {false, nullptr};
    }

    // Set the start time.
    fprintf(stderr, "    SAddSampleAction: setting start time\n");
    fflush(stderr);
    cutLink->setStartTime(timePos_);

    // Now parent the link to the track (safe: SLink is fully constructed).
    fprintf(stderr, "    SAddSampleAction: setting parent\n");
    fflush(stderr);
    cutLink->setParent(track);

    // Find the newly created clip in the track's children to get its index.
    fprintf(stderr, "    SAddSampleAction: getting children\n");
    fflush(stderr);
    const QObjectList &children = track->children();
    fprintf(stderr, "    SAddSampleAction: finding clip index (children count=%d)\n", children.size());
    fflush(stderr);
    int clipIndex = children.indexOf(cutLink);
    fprintf(stderr, "    SAddSampleAction: clipIndex=%d\n", clipIndex);
    fflush(stderr);

    // TEMP: Return nullptr for inverse to debug crash
    fprintf(stderr, "    SAddSampleAction: returning no inverse (debugging)\n");
    fflush(stderr);
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

// DISABLED: Self-register the action type
// static const bool s_reg_addsample = (
//     SActionRegistry::instance().registerType(
//         QStringLiteral("add-sample"),
//         []{ return new SAddSampleAction; }
//     ), true
// );
