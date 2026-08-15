#include "app/objects/cut/ssplitclipaction.h"
#include "app/model/sobjectpath.h"
#include "app/model/splacements.h"
#include "app/objects/cut/sunsplitclipaction.h"
#include "app/model/sproject.h"
#include "app/actions/sactionregistry.h"
#include "app/model/slink.h"
#include "app/model/sclipwindow.h"
#include "app/objects/cut/stakestack.h"
#include "app/model/seditgroups.h"
#include "app/actions/scompositeaction.h"
#include "tw/core/twfraction.h"
#include <QDomElement>

using namespace strackpath;

SSplitClipAction::SSplitClipAction(const QList<int> &clipPath, offset_t splitTime,
                                   bool broadcast)
    : clipPath_(clipPath), splitTime_(splitTime), broadcast_(broadcast)
{
}

SApplyResult SSplitClipAction::apply(SProject *project)
{
    if (!project || clipPath_.isEmpty()) {
        return {false, nullptr};
    }
    // Edit-group broadcast: split every member's corresponding clip too.
    if (broadcast_) {
        QList<QList<int>> targets =
            seditgroups::expandClipPaths(project, clipPath_);
        if (targets.size() > 1) {
            SCompositeAction composite;
            for (const QList<int> &p : targets)
                composite.append(new SSplitClipAction(p, splitTime_, false));
            return composite.apply(project);
        }
    }
    SObject *mixer = splacements::rootContainer( project );
    if (!mixer) {
        return {false, nullptr};
    }
    QList<int> trackPath = clipPath_;
    int idx = trackPath.takeLast();
    SObject *track = splacements::laneAt( mixer, trackPath);
    if (!track) {
        return {false, nullptr};
    }
    SLink *link = track->childAt(idx);
    if (!link || (link->getSObject().isPathContainer())) {
        return {false, nullptr};
    }

    offset_t startTime = link->getStartTime();
    offset_t inObjOffset = splitTime_ - startTime;
    SObject &obj0 = link->getSObject();
    // Blocking read (P19): split geometry must never come from the stale
    // try-lock fallback (edit path, bounded block).
    length_t fullDur = obj0.getDurationBlocking();
    if (inObjOffset <= 1 || inObjOffset >= (offset_t)fullDur - 1) {
        return {false, nullptr};        // split point outside the clip
    }

    // A take stack splits into two stacks: every take is split with the
    // plain-window arithmetic below (offsets and durations live in the
    // stretched OUTPUT domain, so the timeline split offset applies to each
    // take as-is, whatever its stretch). Both columns keep the active-take
    // selection — this is what turns takes into per-region comping.
    if (STakeStack *stack = dynamic_cast<STakeStack*>(&obj0)) {
        STakeStack *stack2 = new STakeStack(project);
        for (int i = 0; i < stack->nTakes(); ++i) {
            SClipWindow *w1 = stack->takeAt(i);
            if (!w1) continue;
            SClipWindow *w2 = w1->cloneWindowOver(project);
            if (!w2) continue;
            // Tail anchor = the content position the split offset maps to:
            // exact rational, no floor (proposal 18 Phase 3; W1 makes it
            // piecewise through the warp map, identical to the old /stretch
            // when there are no anchors). The tail carries no loop — a loop
            // is a property of the region that was cut in two.
            w2->setWindowExact( w1->timelineToSourceExact(
                                    Fraction( (int64_t)inObjOffset ) ),
                                fullDur - inObjOffset, 0,
                                w1->stretchOrRate() );
            stack2->insertTake(*w2);
        }
        stack2->setActiveTake(stack->activeTakeIndex());
        stack->setDurationAll(inObjOffset);
        SLink *sl2 = new SLink(*stack2, NULL);
        sl2->setStartTime(startTime + inObjOffset);
        sl2->setParent(track);

        QList<int> firstPath = trackPath;
        firstPath.append(track->indexOfChild(link));
        QList<int> secondPath = trackPath;
        secondPath.append(track->indexOfChild(sl2));
        SAction *inverse = new SUnsplitClipAction(firstPath, secondPath,
                                                  fullDur, inObjOffset);
        return {true, inverse};
    }

    // Ensure the clip is a WINDOW (wrap raw content), replacing the link in
    // place. The window creates its OWN content link (+1 ref on obj0) — the
    // old adopting ctor took `link` itself as content_, and the `delete link`
    // below then left the window's content_ dangling (use-after-free on the
    // next getContent()). Delete the old placement link only AFTER the window
    // holds its ref, so obj0's refcount never touches zero (removeRef()'s
    // deleteLater() cannot be rescinded).
    //
    // Which window type that is comes from the CONTENT's kind (the wrap
    // factory), not from a class-name compare here — this line used to strcmp
    // obj0.metaObject()->className() against the audio window's class name,
    // which no second window type could ever have satisfied.
    SLink *cutLink = link;
    if (!SClipWindow::of(&obj0)) {
        SObject *parentObj = (SObject*)link->parent();
        SClipWindow *wrapped =
            SClipWindow::wrapContent((SProject*)obj0.parent(), obj0);
        if (!wrapped) {
            return {false, nullptr};
        }
        SLink *nlk = new SLink(wrapped->asObject());
        nlk->setStartTime(startTime);
        delete link;
        nlk->setParent(parentObj);
        cutLink = nlk;
    }
    SClipWindow *w1 = SClipWindow::of(&cutLink->getSObject());
    if (!w1) {
        return {false, nullptr};
    }
    length_t origDur = w1->durationBlocking();   // edit path — never stale (P19)

    // Second part: a new window over the same content, starting at the split
    // point. Offsets and durations live in the window's *output* (stretched)
    // frame domain — the domain inObjOffset is measured in — so the tail must
    // carry the head's time scaling; cloneWindowOver copies it (and the grain
    // params) faithfully, and setWindowExact then narrows the copy. Copying
    // through setGrainParams() instead would preserve-span-rescale the
    // duration and double-apply the factor.
    SClipWindow *w2 = w1->cloneWindowOver(project);
    if (!w2) {
        return {false, nullptr};
    }
    // W1: exact rational, no floor, through the window's own map.
    w2->setWindowExact( w1->timelineToSourceExact(
                            Fraction( (int64_t)inObjOffset ) ),
                        origDur - inObjOffset, 0, w1->stretchOrRate() );
    w1->setDurationFromTimeline(inObjOffset);
    SLink *sl2 = new SLink(w2->asObject(), NULL);
    sl2->setStartTime(startTime + inObjOffset);
    sl2->setParent(track);

    // Inverse addresses both parts by their post-split indices.
    QList<int> firstPath = trackPath;  firstPath.append(track->indexOfChild(cutLink));
    QList<int> secondPath = trackPath; secondPath.append(track->indexOfChild(sl2));
    SAction *inverse = new SUnsplitClipAction(firstPath, secondPath, origDur, inObjOffset);
    return {true, inverse};
}

void SSplitClipAction::writeXml(QDomElement &elem) const
{
    elem.setAttribute("clip", pathToString(clipPath_));
    elem.setAttribute("splitTime", QString::fromStdString(Fraction(splitTime_, 1).toString()));
    elem.setAttribute("broadcast", broadcast_ ? 1 : 0);
}

bool SSplitClipAction::readXml(const QDomElement &elem, int /*version*/)
{
    clipPath_ = stringToPath(elem.attribute("clip"));
    splitTime_ = (offset_t)parseFractionOrDouble(elem.attribute("splitTime", "0").toStdString()).toDouble();
    broadcast_ = elem.attribute("broadcast", "1").toInt() != 0;
    return true;
}

static const bool s_reg_splitclip = (
    SActionRegistry::instance().registerType(
        QStringLiteral("split-clip"),
        []{ return new SSplitClipAction; }
    ), true
);
