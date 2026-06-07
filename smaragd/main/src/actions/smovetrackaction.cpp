#include "actions/smovetrackaction.h"
#include "actions/strackpath.h"
#include "sproject.h"
#include "sactionregistry.h"
#include "sstdmixer.h"
#include "strack.h"
#include "slink.h"
#include <QDomElement>

using namespace strackpath;

SMoveTrackAction::SMoveTrackAction(const QList<int> &sourcePath, int toIndex)
    : sourcePath_(sourcePath), toIndex_(toIndex)
{
}

SApplyResult SMoveTrackAction::apply(SProject *project)
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

    // Resolve the parent container and the track's current index within it.
    QList<int> parentPath = sourcePath_;
    int fromIndex = parentPath.takeLast();
    SObject *parent = resolveByPath(root, parentPath);
    if (!parent) {
        return {false, nullptr};
    }
    SLink *link = childLinkAt(parent, fromIndex);
    if (!link) {
        return {false, nullptr};
    }
    if (!dynamic_cast<STrack *>(&link->getSObject())) {
        return {false, nullptr};            // only tracks reorder via this action
    }

    // Clamp the target to a valid slot.
    int n = parent->childCount();
    int toIndex = toIndex_;
    if (toIndex < 0) toIndex = n - 1;
    if (toIndex >= n) toIndex = n - 1;
    if (toIndex == fromIndex) {
        return {true, nullptr};             // nothing to do, nothing to undo
    }

    // Reorder. The mixer also re-wires its (index-assigned) bus inputs; a folder
    // track needs nothing further — twTrackMix sums its children live.
    SStdMixer *mixer = dynamic_cast<SStdMixer *>(parent);
    if (mixer) {
        mixer->reorderTrack(fromIndex, toIndex);   // emits tracksReordered itself
    } else {
        parent->moveChildToIndex(fromIndex, toIndex);
        rootMixer->notifyTreeChanged();            // folder reorder: notify views
    }

    // Inverse: the track now sits at `toIndex`; move it back to `fromIndex`.
    QList<int> newSourcePath = parentPath;
    newSourcePath.append(toIndex);
    SAction *inverse = new SMoveTrackAction(newSourcePath, fromIndex);
    return {true, inverse};
}

void SMoveTrackAction::writeXml(QDomElement &elem) const
{
    elem.setAttribute("source", pathToString(sourcePath_));
    elem.setAttribute("toIndex", toIndex_);
}

bool SMoveTrackAction::readXml(const QDomElement &elem, int /*version*/)
{
    sourcePath_ = stringToPath(elem.attribute("source"));
    toIndex_ = elem.attribute("toIndex", "-1").toInt();
    return true;
}

static const bool s_reg_movetrack = (
    SActionRegistry::instance().registerType(
        QStringLiteral("move-track"),
        []{ return new SMoveTrackAction; }
    ), true
);
