#include "actions/saddtrackaction.h"
#include "actions/sremovetrackaction.h"
#include "sproject.h"
#include "sstdmixer.h"
#include "strack.h"
#include "sapplication.h"
#include <QDomElement>

SAddTrackAction::SAddTrackAction(int index)
    : index_(index)
{
}

SApplyResult SAddTrackAction::apply(SProject *project)
{
    if (!project) {
        return {false, nullptr};
    }

    // Get the root SStdMixer.
    SObject *root = project->getRootComponent();
    SStdMixer *mixer = dynamic_cast<SStdMixer*>(root);
    if (!mixer) {
        return {false, nullptr};
    }

    // Create a new track and append it (insertTrack is append-only).
    STrack *track = new STrack(project);
    mixer->insertTrack(*track);
    int landing = mixer->getNTracks() - 1;

    // Honour a requested index by moving the appended track into place
    // (-1 or out-of-range = leave at the end).
    int actualIndex = (index_ < 0 || index_ > landing) ? landing : index_;
    if (actualIndex != landing) {
        mixer->reorderTrack(landing, actualIndex);
    }

    // Rewire speaker so new track is audible.
    SApplication::app().rewireSpeaker();

    // Create inverse action: remove at the same index.
    SAction *inverse = new SRemoveTrackAction(actualIndex);

    return {true, inverse};
}

void SAddTrackAction::writeXml(QDomElement &elem) const
{
    elem.setAttribute("index", index_);
}

bool SAddTrackAction::readXml(const QDomElement &elem, int /*version*/)
{
    index_ = elem.attribute("index", "-1").toInt();
    return true;
}
