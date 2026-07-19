#include "app/objects/cut/ssplitclipaction.h"
#include "app/model/sobjectpath.h"
#include "app/model/splacements.h"
#include "app/objects/cut/sunsplitclipaction.h"
#include "app/model/sproject.h"
#include "app/actions/sactionregistry.h"
#include "app/model/slink.h"
#include "app/objects/cut/scut.h"
#include "app/objects/cut/stakestack.h"
#include "app/model/seditgroups.h"
#include "app/actions/scompositeaction.h"
#include "tw/core/twfraction.h"
#include <QDomElement>
#include <cstring>

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

    // A take stack splits into two stacks: every take cut is split with the
    // plain-cut arithmetic below (offsets and durations live in the stretched
    // OUTPUT domain, so the timeline split offset applies to each take as-is,
    // whatever its stretch). Both columns keep the active-take selection —
    // this is what turns takes into per-region comping.
    if (STakeStack *stack = dynamic_cast<STakeStack*>(&obj0)) {
        STakeStack *stack2 = new STakeStack(project);
        for (int i = 0; i < stack->nTakes(); ++i) {
            SCut *c1 = stack->takeCutAt(i);
            if (!c1) continue;
            SCut *c2 = new SCut(project, c1->getContent());
            c2->setGrainParamsRaw(c1->getGrainParams());
            // Tail anchor = head anchor + split offset mapped to source:
            // exact rational, no floor (proposal 18 Phase 3).
            c2->setSrcStartRaw( c1->getSrcStart()
                + ( c1->getStretchExact() > Fraction(0)
                        ? Fraction( (int64_t)inObjOffset ) / c1->getStretchExact()
                        : Fraction( (int64_t)inObjOffset ) ) );
            c2->setDuration(fullDur - inObjOffset);
            stack2->insertTake(*c2);
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

    // Ensure the clip is an SCut (wrap a raw link), replacing the link in place.
    SLink *cutLink = link;
    if (strcmp(obj0.metaObject()->className(), "SCut") != 0) {
        SObject *parentObj = (SObject*)link->parent();
        SCut *sc = new SCut((SProject*)obj0.parent(), *link);
        SLink *nlk = new SLink(*sc);
        nlk->setStartTime(startTime);
        delete link;
        nlk->setParent(parentObj);
        cutLink = nlk;
    }
    SCut *sc1 = dynamic_cast<SCut*>(&cutLink->getSObject());
    if (!sc1) {
        return {false, nullptr};
    }
    Fraction sc1Anchor = sc1->getSrcStart();
    length_t origDur = sc1->getDurationBlocking();   // edit path — never stale (P19)

    // Second part: a new cut over the same content, starting at the split point.
    // startOffset_ and cutDuration_ both live in the grain source's *output*
    // (stretched) frame domain — the same domain inObjOffset is measured in — so
    // the offset arithmetic below is correct only if sc2 carries the same stretch
    // as sc1. Inherit the full grain params (stretch + pitch) via the Raw setter:
    // setGrainParams() would rescale offset/duration by the stretch ratio (it
    // assumes you are *changing* stretch on an existing clip), which would
    // double-apply the factor here. setStartOffset/setDuration below rebuild the
    // reader with these params in place.
    SCut *sc2 = new SCut(project, sc1->getContent());
    sc2->setGrainParamsRaw(sc1->getGrainParams());
    sc2->setSrcStartRaw( sc1Anchor
        + ( sc1->getStretchExact() > Fraction(0)
                ? Fraction( (int64_t)inObjOffset ) / sc1->getStretchExact()
                : Fraction( (int64_t)inObjOffset ) ) );
    sc2->setDuration(origDur - inObjOffset);
    sc1->setDuration(inObjOffset);
    SLink *sl2 = new SLink(*sc2, NULL);
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
