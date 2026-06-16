#include "actions/sunsplitclipaction.h"
#include "actions/ssplitclipaction.h"
#include "actions/strackpath.h"
#include "sproject.h"
#include "sstdmixer.h"
#include "strack.h"
#include "slink.h"
#include "scut.h"
#include "twfraction.h"
#include <QDomElement>

using namespace strackpath;

SUnsplitClipAction::SUnsplitClipAction(const QList<int> &firstPath,
                                       const QList<int> &secondPath,
                                       length_t restoreDuration, offset_t inObjOffset)
    : firstPath_(firstPath), secondPath_(secondPath),
      restoreDuration_(restoreDuration), inObjOffset_(inObjOffset)
{
}

SApplyResult SUnsplitClipAction::apply(SProject *project)
{
    if (!project || firstPath_.isEmpty() || secondPath_.isEmpty()) {
        return {false, nullptr};
    }
    SStdMixer *mixer = dynamic_cast<SStdMixer*>(project->getRootComponent());
    if (!mixer) {
        return {false, nullptr};
    }

    // Delete the second part first. It was appended (higher index than the
    // first), so removing it does not shift the first part's index.
    QList<int> stp = secondPath_;
    int sidx = stp.takeLast();
    STrack *sTrack = dynamic_cast<STrack*>(resolveByPath(mixer, stp));
    if (sTrack) {
        if (SLink *sl2 = sTrack->childAt(sidx)) {
            delete sl2;     // its SCut becomes unreferenced -> deleteLater
        }
    }

    // Restore the first part's pre-split length.
    QList<int> ftp = firstPath_;
    int fidx = ftp.takeLast();
    STrack *fTrack = dynamic_cast<STrack*>(resolveByPath(mixer, ftp));
    SLink *first = fTrack ? fTrack->childAt(fidx) : nullptr;
    if (!first) {
        return {false, nullptr};
    }
    offset_t startTime = first->getStartTime();
    if (SCut *sc1 = dynamic_cast<SCut*>(&first->getSObject())) {
        sc1->setDuration(restoreDuration_);
    }

    // Inverse: re-split at the same point.
    SAction *inverse = new SSplitClipAction(firstPath_, startTime + inObjOffset_);
    return {true, inverse};
}

void SUnsplitClipAction::writeXml(QDomElement &elem) const
{
    elem.setAttribute("first", pathToString(firstPath_));
    elem.setAttribute("second", pathToString(secondPath_));
    elem.setAttribute("restoreDuration", QString::fromStdString(Fraction(restoreDuration_, 1).toString()));
    elem.setAttribute("inObjOffset", QString::fromStdString(Fraction(inObjOffset_, 1).toString()));
}

bool SUnsplitClipAction::readXml(const QDomElement & /*elem*/, int /*version*/)
{
    return false;   // created live as a split's inverse; not reconstructed here
}
