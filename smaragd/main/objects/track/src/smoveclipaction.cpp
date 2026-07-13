#include "app/objects/track/smoveclipaction.h"
#include "app/model/splacements.h"
#include "app/model/seditgroups.h"
#include "app/actions/scompositeaction.h"
#include "app/objects/track/strackpath.h"
#include "app/model/sproject.h"
#include "app/actions/sactionregistry.h"
#include "app/objects/track/strack.h"
#include "app/model/slink.h"
#include "tw/core/twfraction.h"
#include <QDomElement>

using namespace strackpath;

SMoveClipAction::SMoveClipAction(const QList<int> &clipPath,
                                 const QList<int> &destTrackPath,
                                 offset_t newStartTime,
                                 bool broadcast)
    : clipPath_(clipPath), destTrackPath_(destTrackPath),
      newStartTime_(newStartTime), broadcast_(broadcast)
{
}

SApplyResult SMoveClipAction::apply(SProject *project)
{
    if (!project || clipPath_.isEmpty()) {
        return {false, nullptr};
    }
    SObject *root = splacements::rootContainer( project );
    SObject *mixer = root;
    if (!mixer) {
        return {false, nullptr};
    }

    // Edit-group broadcast — same-track moves only: each member's
    // corresponding clip moves to the same start time within its OWN lane.
    if (broadcast_) {
        QList<int> srcLane = clipPath_;
        srcLane.removeLast();
        if (srcLane == destTrackPath_) {
            QList<QList<int>> targets =
                seditgroups::expandClipPaths(project, clipPath_);
            if (targets.size() > 1) {
                SCompositeAction composite;
                for (const QList<int> &p : targets) {
                    QList<int> lane = p;
                    lane.removeLast();
                    composite.append(new SMoveClipAction(
                        p, lane, newStartTime_, false));
                }
                return composite.apply(project);
            }
        }
    }

    // Resolve the clip: its track (path minus last) and its link index.
    QList<int> srcTrackPath = clipPath_;
    int clipIdx = srcTrackPath.takeLast();
    SObject *srcTrack = resolveByPath(root, srcTrackPath);
    if (!srcTrack) {
        return {false, nullptr};
    }
    SLink *link = srcTrack->childAt(clipIdx);
    if (!link || (link->getSObject().isPathContainer())) {
        return {false, nullptr};        // missing, or it's a track lane not a clip
    }

    SObject *destObj = resolveByPath(root, destTrackPath_);
    STrack *destTrack = dynamic_cast<STrack*>(destObj);
    if (!destTrack) {
        return {false, nullptr};
    }

    // Capture the pre-move placement for the inverse.
    offset_t oldStart = link->getStartTime();

    if (destObj != srcTrack) {
        link->setParent(destTrack);     // the link persists; no refcount change
    }
    link->setStartTime(newStartTime_);

    // Inverse: move it from where it is now back to its old track + start.
    QList<int> newClipPath = destTrackPath_;
    newClipPath.append(destTrack->indexOfChild(link));
    SAction *inverse = new SMoveClipAction(newClipPath, srcTrackPath, oldStart);
    return {true, inverse};
}

void SMoveClipAction::writeXml(QDomElement &elem) const
{
    elem.setAttribute("clip", pathToString(clipPath_));
    elem.setAttribute("destTrack", pathToString(destTrackPath_));
    elem.setAttribute("startTime", QString::fromStdString(Fraction(newStartTime_, 1).toString()));
    elem.setAttribute("broadcast", broadcast_ ? 1 : 0);
}

bool SMoveClipAction::readXml(const QDomElement &elem, int /*version*/)
{
    clipPath_ = stringToPath(elem.attribute("clip"));
    destTrackPath_ = stringToPath(elem.attribute("destTrack"));
    newStartTime_ = (offset_t)parseFractionOrDouble(elem.attribute("startTime", "0").toStdString()).toDouble();
    broadcast_ = elem.attribute("broadcast", "1").toInt() != 0;
    return true;
}

static const bool s_reg_moveclip = (
    SActionRegistry::instance().registerType(
        QStringLiteral("move-clip"),
        []{ return new SMoveClipAction; }
    ), true
);
