#include "app/objects/track/sreparenttrackaction.h"
#include "app/objects/track/strackpath.h"
#include "app/model/sproject.h"
#include "app/actions/sactionregistry.h"
#include "app/objects/mixer/sstdmixer.h"
#include "app/objects/track/strack.h"
#include "app/model/slink.h"
#include "app/model/sappcontext.h"
#include <QDomElement>

using namespace strackpath;

SReparentTrackAction::SReparentTrackAction(const QList<int> &sourcePath,
                                           const QList<int> &destParentPath,
                                           int destIndex)
    : sourcePath_(sourcePath), destParentPath_(destParentPath), destIndex_(destIndex)
{
}

SApplyResult SReparentTrackAction::apply(SProject *project)
{
    if (!project) {
        return {false, nullptr};
    }

    SObject *root = project->getRootComponent();
    SStdMixer *rootMixer = dynamic_cast<SStdMixer *>(root);
    if (!rootMixer) {
        return {false, nullptr};
    }

    if (sourcePath_.isEmpty()) {
        return {false, nullptr};            // the root is not movable
    }

    // Resolve the source: its parent container, its index in that parent, and
    // the track itself.
    QList<int> sourceParentPath = sourcePath_;
    int sourceIndex = sourceParentPath.takeLast();
    SObject *sourceParent = resolveByPath(root, sourceParentPath);
    if (!sourceParent) {
        return {false, nullptr};
    }
    SLink *sourceLink = childLinkAt(sourceParent, sourceIndex);
    if (!sourceLink) {
        return {false, nullptr};
    }
    STrack *track = dynamic_cast<STrack *>(&sourceLink->getSObject());
    if (!track) {
        return {false, nullptr};            // only tracks reparent
    }

    // Resolve the destination container.
    SObject *destParent = resolveByPath(root, destParentPath_);
    if (!destParent) {
        return {false, nullptr};
    }
    SStdMixer *destMixer = dynamic_cast<SStdMixer *>(destParent);
    STrack   *destTrack = dynamic_cast<STrack *>(destParent);
    if (!destMixer && !destTrack) {
        return {false, nullptr};            // not a container we can attach into
    }

    // Reject a no-op / pure reorder (that is SMoveTrackAction's job) and cycles.
    if (destParent == sourceParent) {
        return {false, nullptr};
    }
    if (isSelfOrDescendant(destParent, track)) {
        return {false, nullptr};
    }

    SStdMixer *srcMixer = dynamic_cast<SStdMixer *>(sourceParent);
    STrack    *srcTrack = dynamic_cast<STrack *>(sourceParent);

    // Pin the track across the move: detaching drops its old link's reference,
    // and removeRef() -> deleteLater() is irreversible, so the count must never
    // touch zero between detach and re-attach.
    track->addRef();

    // --- detach from the old parent ---
    if (srcMixer) {
        srcMixer->removeTrack(*sourceLink);            // disconnects + deletes link + rewires
        // removeTrack(SLink&) does not drop mute/solo connections; since the
        // track survives, clear every remaining track->old-mixer connection.
        QObject::disconnect(track, nullptr, srcMixer, nullptr);
    } else {
        delete sourceLink;                             // childObjectRemoved -> duration update
        if (srcTrack) {
            QObject::disconnect(track, nullptr, srcTrack, nullptr);
        }
    }
    sourceLink = nullptr;

    // --- attach to the new parent (append, then move into the exact slot) ---
    if (destMixer) {
        destMixer->insertTrack(*track);                // appends + connects + rewires
        int landing = destMixer->getNTracks() - 1;
        if (destIndex_ >= 0 && destIndex_ < landing) {
            destMixer->reorderTrack(landing, destIndex_);
        }
    } else {
        // SLink ctor note: build with no parent, then setParent so childEvent
        // fires on a fully-constructed link.
        SLink *nl = new SLink(*track, nullptr);
        nl->setParent(destTrack);                      // appends
        int landing = destTrack->childCount() - 1;
        if (destIndex_ >= 0 && destIndex_ < landing) {
            destTrack->moveChildToIndex(landing, destIndex_);
        }
    }

    // The new link now holds the reference; release our pin.
    track->removeRef();

    // Refresh the engine graph root (mirrors SAddTrackAction).
    SAppContext::get().rewireSpeaker();

    // Detach fired a mid-operation view refresh (track briefly parentless) and
    // the folder-side attach emits no mixer signal; announce the final tree so
    // views rebuild against the completed state.
    rootMixer->notifyTreeChanged();

    // Synthesize the inverse from the *post-move* tree so it is immune to the
    // index shifts the move caused — and restore the exact original slot.
    QList<int> newSourcePath     = pathOf(root, track);
    QList<int> invDestParentPath = pathOf(root, sourceParent);
    SAction *inverse = new SReparentTrackAction(newSourcePath, invDestParentPath, sourceIndex);

    return {true, inverse};
}

void SReparentTrackAction::writeXml(QDomElement &elem) const
{
    elem.setAttribute("source", pathToString(sourcePath_));
    elem.setAttribute("destParent", pathToString(destParentPath_));
    elem.setAttribute("destIndex", destIndex_);
}

bool SReparentTrackAction::readXml(const QDomElement &elem, int /*version*/)
{
    sourcePath_ = stringToPath(elem.attribute("source"));
    destParentPath_ = stringToPath(elem.attribute("destParent"));
    destIndex_ = elem.attribute("destIndex", "-1").toInt();
    return true;
}

static const bool s_reg_reparenttrack = (
    SActionRegistry::instance().registerType(
        QStringLiteral("reparent-track"),
        []{ return new SReparentTrackAction; }
    ), true
);
