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

    // Calculate actual index (handle -1 = append).
    int actualIndex = (index_ == -1) ? mixer->getNTracks() : index_;

    // Create a new track.
    STrack *track = new STrack(project);

    // Insert into mixer.
    mixer->insertTrack(actualIndex, *track);

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
