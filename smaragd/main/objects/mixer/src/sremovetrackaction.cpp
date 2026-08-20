#include "app/objects/mixer/sremovetrackaction.h"
#include "app/objects/mixer/srestoretrackaction.h"
#include "app/model/sproject.h"
#include "app/objects/mixer/sstdmixer.h"
#include "app/objects/track/strack.h"
#include "app/model/slink.h"
#include "app/model/splacements.h"
#include "app/model/sobjectpath.h"
#include "app/model/sappcontext.h"
#include "app/actions/sactionregistry.h"
#include <QDomElement>

SRemoveTrackAction::SRemoveTrackAction(const QList<int> &trackPath)
    : trackPath_(trackPath)
{
}

SRemoveTrackAction::~SRemoveTrackAction()
{
    // If this command is discarded while still holding the removed track (i.e.
    // we are in the "removed" state), let it go — its refcount hits zero and it
    // (with its subtree) is finally torn down.
    dropStalePin();
}

void SRemoveTrackAction::dropStalePin()
{
    if (holdsRef_ && heldTrack_) {
        heldTrack_->removeRef();
    }
    heldTrack_ = nullptr;
    holdsRef_ = false;
}

void SRemoveTrackAction::releaseHeld()
{
    dropStalePin();
}

SApplyResult SRemoveTrackAction::apply(SProject *project)
{
    if (!project || trackPath_.isEmpty()) {
        return {false, nullptr};          // the root is not removable
    }
    SObject *root = splacements::rootNamed( project, pathRoot_ );
    SStdMixer *rootMixer = dynamic_cast<SStdMixer*>(root);
    if (!rootMixer) {
        return {false, nullptr};
    }

    // Split the path into "the container" and "our index inside it". The
    // container is the root mixer for a top-level track and a folder STrack for
    // a nested one; both are lanes, and both are legal here.
    QList<int> parentPath = trackPath_;
    const int index = parentPath.takeLast();
    SObject *parent = splacements::laneAt(root, parentPath);
    if (!parent) {
        return {false, nullptr};
    }

    SLink *link = parent->childAt(index);
    if (!link) {
        return {false, nullptr};
    }
    STrack *track = dynamic_cast<STrack*>(&link->getSObject());
    if (!track) {
        return {false, nullptr};          // that child is a clip, not a lane
    }

    // Release any pin left from a previous apply whose track is now orphaned
    // (e.g. a redo that re-created the track elsewhere).
    dropStalePin();

    // Pin the track on THIS action so it (and its subtree) survives the removal;
    // the undo command reuses this object, so the pin persists across redo.
    track->addRef();
    heldTrack_ = track;
    holdsRef_ = true;

    // Detach. The mixer owns its track list (removeTrack also rewires the
    // summing inputs and drops the mixer-side connections); a folder just holds
    // an ordinary SLink child. Same split as SReparentTrackAction's detach half,
    // including the disconnect: the track SURVIVES here, so a stale
    // track -> old-parent connection would keep firing at a container that no
    // longer sums it.
    if (SStdMixer *parentMixer = dynamic_cast<SStdMixer*>(parent)) {
        parentMixer->removeTrack(*link);
        QObject::disconnect(track, nullptr, parentMixer, nullptr);
    } else {
        STrack *parentTrack = dynamic_cast<STrack*>(parent);
        delete link;
        if (parentTrack) {
            QObject::disconnect(track, nullptr, parentTrack, nullptr);
        }
    }

    // Removing a lane changes what every OTHER lane hears when solo is in force
    // (it may have been the soloed one), and a folder's summing inputs moved.
    SAppContext::get().rewireSpeaker();
    rootMixer->applyAudibility();
    rootMixer->notifyTreeChanged();

    return {true, new SRestoreTrackAction(this, parentPath, index)};
}

void SRemoveTrackAction::writeXml(QDomElement &elem) const
{
    elem.setAttribute("trackPath", strackpath::qualifiedToString( pathRoot_, trackPath_ ));
}

bool SRemoveTrackAction::readXml(const QDomElement &elem, int /*version*/)
{
    // Sniff the spelling: pre-existing scripts carry no version attribute, and
    // the legacy `index` is exactly a one-element path.
    trackPath_ = elem.hasAttribute("trackPath")
        ? strackpath::parseInto( pathRoot_, elem.attribute("trackPath") )
        : QList<int>{ elem.attribute("index", "0").toInt() };
    return true;
}

static const bool s_reg_removetrack = (
    SActionRegistry::instance().registerType(
        QStringLiteral("remove-track"),
        []{ return new SRemoveTrackAction; }
    ), true
);
