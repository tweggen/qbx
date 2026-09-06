#include "app/objects/mixer/smovetrackaction.h"
#include "app/objects/track/strackpath.h"
#include "app/model/sproject.h"
#include "app/actions/sactionregistry.h"
#include "app/objects/mixer/sstdmixer.h"
#include "app/objects/track/strack.h"
#include "app/model/slink.h"
#include "app/model/splacements.h"
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

    // THE ARRANGEMENT QUALIFIER IS HONOURED. `parseInto` peels a "Name:"
    // qualifier off the path into pathRoot_, and this resolved against
    // project->getRootComponent() -- THE DEFAULT ROOT -- unconditionally, so
    // `source="Drums:0"` silently moved the DEFAULT arrangement's track 0.
    // That is the "resolved against the wrong root SUCCEEDS" class
    // sobjectpath.h's own comment warns about: no error, no log line, the
    // wrong object edited. rootNamed() returns the default root for an empty
    // qualifier, so an unqualified path is unchanged, and NULL for a name that
    // is not an arrangement -- a refusal rather than a silent misfire.
    SObject *root = splacements::rootNamed( project, pathRoot_ );
    SStdMixer *rootMixer = dynamic_cast<SStdMixer *>(root);
    if (!rootMixer) {
        return {false, nullptr};
    }

    if (sourcePath_.isEmpty()) {
        return {false, nullptr};            // the root is not movable
    }

    // AC5.2 / D6: A SYSTEM LANE IS NOT THE USER'S OBJECT TO RESTRUCTURE.
    //
    // ASKED BEFORE THE PARENT/INDEX SPLIT, and that ordering is the whole
    // point. These three verbs address a track as "its parent, plus its index
    // in that parent", and a master lane is deliberately NOT a childLinks()
    // member (D2) -- so `$master` splits into an empty parent path and the
    // index -1, childAt(-1) answers null, and the verb already refuses. It
    // refuses SILENTLY, by an accident of the sentinel's numeric value, and
    // that accident stops holding the moment M7 gives a send lane a name form
    // or M6 nests a conductor lane one level down. Asking the question
    // explicitly, of the resolved object, is what makes the refusal a POLICY
    // rather than a coincidence -- and it is what announces it (D6: "a bound
    // is ANNOUNCED, never silent").
    if( splacements::refuseSystemLane(
            resolveByPath( root, sourcePath_ ), "move-track" ) ) {
        return {false, nullptr};
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
    sourcePath_ = parseInto( pathRoot_, elem.attribute("source") );
    toIndex_ = elem.attribute("toIndex", "-1").toInt();
    return true;
}

static const bool s_reg_movetrack = (
    SActionRegistry::instance().registerType(
        QStringLiteral("move-track"),
        []{ return new SMoveTrackAction; }
    ), true
);
