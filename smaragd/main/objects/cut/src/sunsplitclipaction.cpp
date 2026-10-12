#include "app/objects/cut/sunsplitclipaction.h"
#include "app/objects/cut/stakestack.h"
#include "app/objects/cut/stakehelpers.h"
#include "app/model/sobjectpath.h"
#include "app/model/splacements.h"
#include "app/objects/cut/ssplitclipaction.h"
#include "app/model/sproject.h"
#include "app/model/slink.h"
#include "tw/core/twfraction.h"
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
    SObject *mixer = splacements::rootNamed( project, pathRoot_ );
    if (!mixer) {
        return {false, nullptr};
    }

    // BOTH PARTS ARE RESOLVED FIRST, because the take-column restore below
    // reads takes OUT of the tail before the tail dies (plan 53 §4.1).
    QList<int> stp = secondPath_;
    int sidx = stp.takeLast();
    SObject *sTrack = splacements::laneAt( mixer, stp);
    SLink *sl2 = sTrack ? sTrack->childAt(sidx) : nullptr;

    QList<int> ftp = firstPath_;
    int fidx = ftp.takeLast();
    SObject *fTrack = splacements::laneAt( mixer, ftp);
    SLink *first = fTrack ? fTrack->childAt(fidx) : nullptr;
    if (!first) {
        return {false, nullptr};
    }
    offset_t startTime = first->getStartTime();

    STakeStack *head = dynamic_cast<STakeStack *>( &first->getSObject() );
    STakeStack *tail = sl2 ? dynamic_cast<STakeStack *>( &sl2->getSObject() )
                           : nullptr;

    if( hasTakes_ && head ) {
        // THE MIXED RESTORE (plan 53 §4.1). The column is its full length
        // again first -- directly, because every take's extent is about to be
        // written from the record and setDurationAll's clamp would be a second
        // opinion about geometry this code already knows.
        head->setColumnDuration( restoreDuration_ );

        // ONE PASS IN ASCENDING origIndex, which reconstructs the pre-split
        // ORDER: after k records the head holds exactly those k takes at
        // indices 0..k-1, so record k's take belongs at index k -- already
        // there when it never left the head, inserted there when it comes back
        // from the tail. (Head-resident takes keep their relative order
        // through the split, which is what makes "already there" true.)
        //
        // `removedFromTail` is the matching bookkeeping on the other side:
        // tail indices ascend with origIndex, so each removal shifts the ones
        // still to come down by one.
        int k = 0, removedFromTail = 0;
        for( const TakeRestore &tr : takes_ ) {
            if( tr.fromTail ) {
                if( tail ) {
                    stakes::moveTakeBetweenColumns(
                        *tail, tr.halfIndex - removedFromTail,
                        *head, k, tr.tStart, tr.tLen );
                    ++removedFromTail;
                }
            } else {
                // Trimmed (a crossing take) or untouched (a head-only one):
                // either way its extent is restored, and for an untouched take
                // that is a no-op by construction.
                head->setTakeExtent( k, tr.tStart, tr.tLen );
            }
            ++k;
        }
    } else {
        // Restore the first part's pre-split length.
        //
        // setDuration is virtual: a window object restores its window; a take
        // stack (STakeStack) forwards to every take (setDurationAll). Generic
        // on purpose — unsplit never needed to know the window TYPE, and for
        // a column with no per-take record (a scripted unsplit-clip, or a
        // WRAPPED column, whose split clones the whole thing) it still does
        // not.
        first->getSObject().setDuration(restoreDuration_);
    }

    // Put the pre-split comp map back (plan 53 B3), AFTER the duration: a map
    // is read by position within the column, so the column must be its full
    // length again before the map describes it.
    if( hasRestoreMap_ && head ) {
        head->setCompMap( restoreMap_ );
    }
    if( hasTakes_ && head ) {
        if( restoreActive_ >= -1 ) head->setActiveTake( restoreActive_ );
        // ONE PUBLISH whatever happened above, for the reason the split's own
        // tail-end call spells: setColumnDuration emits nothing, setActiveTake
        // returns early when the index did not change, and an empty map is no
        // change to setCompMap. At the length the column already has this is a
        // geometric no-op whose only effect is the track's resync.
        head->setDurationAll( restoreDuration_ );
    }

    // THE TAIL DIES LAST (plan 53 §4.1): its moved-out takes have been taken
    // back above, and what is left of it -- the CLONES of the crossing takes
    // -- is exactly what should go. Deleting it does not shift the first
    // part's index, because it was appended and so sits above it.
    if( sl2 ) delete sl2;   // its column becomes unreferenced -> deleteLater

    // Inverse: re-split at the same point.
    SAction *inverse = new SSplitClipAction(firstPath_, startTime + inObjOffset_);
    return {true, inverse};
}

void SUnsplitClipAction::writeXml(QDomElement &elem) const
{
    elem.setAttribute("first", qualifiedToString( pathRoot_, firstPath_ ));
    elem.setAttribute("second", qualifiedToString( pathRoot_, secondPath_ ));
    elem.setAttribute("restoreDuration", QString::fromStdString(Fraction(restoreDuration_, 1).toString()));
    elem.setAttribute("inObjOffset", QString::fromStdString(Fraction(inObjOffset_, 1).toString()));
}

bool SUnsplitClipAction::readXml(const QDomElement & /*elem*/, int /*version*/)
{
    return false;   // created live as a split's inverse; not reconstructed here
}
