#include "actions/sremovetrackaction.h"
#include "actions/srestoretrackaction.h"
#include "sproject.h"
#include "sstdmixer.h"
#include "strack.h"
#include "slink.h"
#include "sactionregistry.h"
#include <QDomElement>

SRemoveTrackAction::SRemoveTrackAction(int index)
    : index_(index)
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
    if (!project) {
        return {false, nullptr};
    }
    SStdMixer *mixer = dynamic_cast<SStdMixer*>(project->getRootComponent());
    if (!mixer) {
        return {false, nullptr};
    }

    // Release any pin left from a previous apply whose track is now orphaned
    // (e.g. a redo that re-created the track elsewhere).
    dropStalePin();

    SLink *link = mixer->getTrackAt(index_);
    if (!link) {
        return {false, nullptr};
    }
    STrack *track = dynamic_cast<STrack*>(&link->getSObject());
    if (!track) {
        return {false, nullptr};
    }

    // Pin the track on THIS action so it (and its subtree) survives the removal;
    // the undo command reuses this object, so the pin persists across redo.
    track->addRef();
    heldTrack_ = track;
    holdsRef_ = true;

    mixer->removeTrack(index_);

    return {true, new SRestoreTrackAction(this, index_)};
}

void SRemoveTrackAction::writeXml(QDomElement &elem) const
{
    elem.setAttribute("index", index_);
}

bool SRemoveTrackAction::readXml(const QDomElement &elem, int /*version*/)
{
    index_ = elem.attribute("index", "0").toInt();
    return true;
}

static const bool s_reg_removetrack = (
    SActionRegistry::instance().registerType(
        QStringLiteral("remove-track"),
        []{ return new SRemoveTrackAction; }
    ), true
);
