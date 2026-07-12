#include "app/objects/mixer/srestoretrackaction.h"
#include "app/objects/mixer/sremovetrackaction.h"
#include "app/model/sproject.h"
#include "app/objects/mixer/sstdmixer.h"
#include "app/objects/track/strack.h"
#include "app/model/sappcontext.h"
#include <QDomElement>

SRestoreTrackAction::SRestoreTrackAction(SRemoveTrackAction *owner, int index)
    : owner_(owner), index_(index)
{
}

SApplyResult SRestoreTrackAction::apply(SProject *project)
{
    if (!project || !owner_) {
        return {false, nullptr};
    }
    SStdMixer *mixer = dynamic_cast<SStdMixer*>(project->getRootComponent());
    if (!mixer) {
        return {false, nullptr};
    }
    STrack *track = owner_->heldTrack();
    if (!track) {
        return {false, nullptr};      // nothing pinned to restore
    }

    // Re-attach (a new SLink takes a reference), position at the original index,
    // then release the pin the remove action was holding.
    mixer->insertTrack(*track);
    int landing = mixer->getNTracks() - 1;
    if (index_ >= 0 && index_ < landing) {
        mixer->reorderTrack(landing, index_);
    }
    owner_->releaseHeld();

    SAppContext::get().rewireSpeaker();
    mixer->notifyTreeChanged();

    return {true, new SRemoveTrackAction(index_)};
}

void SRestoreTrackAction::writeXml(QDomElement &elem) const
{
    // Never serialized standalone (created live as a remove's inverse); record
    // the index for completeness.
    elem.setAttribute("index", index_);
}

bool SRestoreTrackAction::readXml(const QDomElement & /*elem*/, int /*version*/)
{
    return false;   // cannot be reconstructed from XML (no pinned track)
}
