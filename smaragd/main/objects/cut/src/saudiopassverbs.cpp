#include "app/objects/cut/saudiopassverbs.h"

#include "app/objects/cut/saddtakeaction.h"
#include "app/objects/cut/smergecolumnsactions.h"
#include "app/objects/cut/splaceclipaction.h"
#include "app/objects/cut/sremovesampleaction.h"
#include "app/objects/cut/ssetclipnameaction.h"
#include "app/objects/cut/ssplitclipaction.h"

SAction *SAudioPassVerbs::placePass( const QList<int> &lanePath,
                                     const srecplan::Span &span,
                                     offset_t at, length_t len ) const
{
    // THE ONE PIECE OF SOURCE ARITHMETIC in the audio plan: `at` frames into
    // the timeline is `at - a` frames into the pass, which is
    // `srcOffset + (at - a)` frames into the file. A negative result is legal
    // and means leading silence (proposal 23).
    return new SPlaceClipAction( lanePath, span.filePath, at,
                                 span.srcOffset + at - span.a, len );
}

SAction *SAudioPassVerbs::addTakeFromPass( const QList<int> &clipPath,
                                           const srecplan::Span &span,
                                           offset_t takeStart ) const
{
    // Same conversion, for a take anchor. `takeStart` may sit up to two frames
    // BEFORE the pass began, when a cut that close to a child's own edge was
    // refused (`pieceCanSplitAt`), so this anchor legitimately goes slightly
    // negative — leading silence again, and the reason the planner may hand
    // over a position outside `[a, b)` at all.
    return new SAddTakeAction( clipPath, span.filePath,
                               span.srcOffset + takeStart - span.a );
}

SAction *SAudioPassVerbs::splitClip( const QList<int> &clipPath,
                                     offset_t at ) const
{
    // broadcast=0 (plan 53 §5.1): a recording pass decides the edit for the
    // lanes the RECORDER armed, which it plans one by one against each lane's
    // own snapshot. Letting the cut fan out through the edit group would cut
    // lanes this pass never looked at, and cut an armed member twice.
    return new SSplitClipAction( clipPath, at, false );
}

SAction *SAudioPassVerbs::removeChild( const QList<int> &lanePath, int index,
                                       offset_t at ) const
{
    // The EMPTY file path is deliberate: `remove-sample` reads the real one
    // off the clip and its own comment calls the caller's value "only a hint".
    // The START is not optional — it is the position the inverse restores to.
    return new SRemoveSampleAction( lanePath, index, QString(), at );
}

SAction *SAudioPassVerbs::nameTake( const QList<int> &clipPath,
                                    const QString &name ) const
{
    // take = -1 is "the ACTIVE take, resolved at apply time", which is the
    // pass in every plan that stacks one and the placement's own window where
    // the pass is a plain clip -- so the plan needs no index of its own.
    //
    // broadcast=0, for the same reason every split in a recording plan carries
    // it (plan 53 §5.1): the pass is planned one lane at a time, and letting
    // the name fan out through the edit group would rename a lane this pass
    // never looked at.
    return new SSetClipNameAction( clipPath, name, -1, false );
}

SAction *SAudioPassVerbs::mergeColumns(
    const QList<QList<int>> &clipPaths, offset_t extentStart,
    length_t extentLen ) const
{
    // `extentLen < 0` is "the operands' union" on both sides of this seam, so
    // the group case (plan 53 D4 (a), QBX-182) is the only one that says
    // anything here and every pre-P4 plan reads identically.
    return new SMergeColumnsAction( clipPaths, extentStart, extentLen );
}
