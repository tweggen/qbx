#include "app/objects/cut/ssplitclipaction.h"
#include "app/objects/cut/sunsplitclipaction.h"
#include "app/objects/track/strackpath.h"
#include "app/model/sproject.h"
#include "app/actions/sactionregistry.h"
#include "app/objects/mixer/sstdmixer.h"
#include "app/objects/track/strack.h"
#include "app/model/slink.h"
#include "app/objects/cut/scut.h"
#include "tw/core/twfraction.h"
#include <QDomElement>
#include <cstring>

using namespace strackpath;

SSplitClipAction::SSplitClipAction(const QList<int> &clipPath, offset_t splitTime)
    : clipPath_(clipPath), splitTime_(splitTime)
{
}

SApplyResult SSplitClipAction::apply(SProject *project)
{
    if (!project || clipPath_.isEmpty()) {
        return {false, nullptr};
    }
    SStdMixer *mixer = dynamic_cast<SStdMixer*>(project->getRootComponent());
    if (!mixer) {
        return {false, nullptr};
    }
    QList<int> trackPath = clipPath_;
    int idx = trackPath.takeLast();
    STrack *track = dynamic_cast<STrack*>(resolveByPath(mixer, trackPath));
    if (!track) {
        return {false, nullptr};
    }
    SLink *link = track->childAt(idx);
    if (!link || dynamic_cast<STrack*>(&link->getSObject())) {
        return {false, nullptr};
    }

    offset_t startTime = link->getStartTime();
    offset_t inObjOffset = splitTime_ - startTime;
    SObject &obj0 = link->getSObject();
    length_t fullDur = obj0.getDuration();
    if (inObjOffset <= 1 || inObjOffset >= (offset_t)fullDur - 1) {
        return {false, nullptr};        // split point outside the clip
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
    offset_t sc1StartOffset = sc1->getStartOffset();
    length_t origDur = sc1->getDuration();

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
    sc2->setStartOffset(sc1StartOffset + inObjOffset);
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
}

bool SSplitClipAction::readXml(const QDomElement &elem, int /*version*/)
{
    clipPath_ = stringToPath(elem.attribute("clip"));
    splitTime_ = (offset_t)parseFractionOrDouble(elem.attribute("splitTime", "0").toStdString()).toDouble();
    return true;
}

static const bool s_reg_splitclip = (
    SActionRegistry::instance().registerType(
        QStringLiteral("split-clip"),
        []{ return new SSplitClipAction; }
    ), true
);
