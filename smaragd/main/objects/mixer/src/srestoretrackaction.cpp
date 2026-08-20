#include "app/objects/mixer/srestoretrackaction.h"
#include "app/objects/mixer/sremovetrackaction.h"
#include "app/model/sproject.h"
#include "app/model/splacements.h"
#include "app/model/sobjectpath.h"
#include "app/model/slink.h"
#include "app/objects/mixer/sstdmixer.h"
#include "app/objects/track/strack.h"
#include "app/model/sappcontext.h"
#include <QDomElement>

SRestoreTrackAction::SRestoreTrackAction(SRemoveTrackAction *owner,
                                         const QList<int> &parentPath, int index)
    : owner_(owner), parentPath_(parentPath), index_(index)
{
}

SApplyResult SRestoreTrackAction::apply(SProject *project)
{
    if (!project || !owner_) {
        return {false, nullptr};
    }
    SObject *root = splacements::rootNamed( project, pathRoot_ );
    SStdMixer *rootMixer = dynamic_cast<SStdMixer*>(root);
    if (!rootMixer) {
        return {false, nullptr};
    }
    SObject *parent = splacements::laneAt(root, parentPath_);
    if (!parent) {
        return {false, nullptr};      // the folder we came out of is gone
    }
    STrack *track = owner_->heldTrack();
    if (!track) {
        return {false, nullptr};      // nothing pinned to restore
    }

    // Re-attach the way the removal detached (see SRemoveTrackAction): the mixer
    // owns its track list, a folder holds an ordinary SLink child. Append, then
    // move into the exact original slot. A new SLink takes a reference, so the
    // pin can be released afterwards.
    if (SStdMixer *parentMixer = dynamic_cast<SStdMixer*>(parent)) {
        parentMixer->insertTrack(*track);
        int landing = parentMixer->getNTracks() - 1;
        if (index_ >= 0 && index_ < landing) {
            parentMixer->reorderTrack(landing, index_);
        }
    } else {
        STrack *parentTrack = dynamic_cast<STrack*>(parent);
        if (!parentTrack) {
            return {false, nullptr};
        }
        // SLink ctor rule: build with no parent, then setParent so childEvent
        // fires on a fully-constructed link.
        SLink *nl = new SLink(*track, nullptr);
        nl->setParent(parentTrack);
        int landing = parentTrack->childCount() - 1;
        if (index_ >= 0 && index_ < landing) {
            parentTrack->moveChildToIndex(landing, index_);
        }
    }
    owner_->releaseHeld();

    SAppContext::get().rewireSpeaker();
    // A restored lane must land with the CURRENT solo/mute picture applied — it
    // may have been removed while something else was soloed.
    rootMixer->applyAudibility();
    rootMixer->notifyTreeChanged();

    QList<int> trackPath = parentPath_;
    trackPath.append(index_);
    return {true, new SRemoveTrackAction(trackPath)};
}

void SRestoreTrackAction::writeXml(QDomElement &elem) const
{
    // Never serialized standalone (created live as a remove's inverse); record
    // the address for completeness.
    elem.setAttribute("parentPath", strackpath::qualifiedToString( pathRoot_, parentPath_ ));
    elem.setAttribute("index", index_);
}

bool SRestoreTrackAction::readXml(const QDomElement & /*elem*/, int /*version*/)
{
    return false;   // cannot be reconstructed from XML (no pinned track)
}
