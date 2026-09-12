#include "app/timeline/strackgestures.h"

#include <QUndoStack>

#include "app/actions/sactionhistory.h"
#include "app/model/slink.h"
#include "app/objects/mixer/saddtrackaction.h"
#include "app/objects/mixer/sremovetrackaction.h"
#include "app/objects/mixer/sreparenttrackaction.h"
#include "app/objects/mixer/sstdmixer.h"
#include "app/objects/mixer/strackbroadcast.h"
#include "app/objects/track/strack.h"
#include "app/objects/track/strackpath.h"
#include "app/shell/sapplication.h"

namespace {

QUndoStack *stack()
{
    SActionHistory *h = SApplication::app().actionHistory();
    return h ? h->undoStack() : nullptr;
}

}  // namespace

namespace strackgestures {

QList<STrack *> structuralTargets( SStdMixer *mixer, STrack *clicked )
{
    if( !mixer || !clicked ) return {};
    return strackbroadcast::pruneNestedTargets(
        strackbroadcast::targetsFor( mixer, clicked ) );
}

bool removeTracks( SStdMixer *mixer, const QList<STrack *> &targets,
                   const Submit &submit )
{
    if( !mixer || targets.isEmpty() || !submit ) return false;

    QUndoStack *st = stack();
    const bool macro = targets.size() > 1 && st;
    if( macro ) st->beginMacro( QStringLiteral( "Remove tracks" ) );
    // Bottom-up: every removal shifts the indices of the lanes below it. The
    // paths are re-derived per step anyway (submit drains synchronously), but
    // going upwards keeps each re-derivation cheap and makes the undo order
    // the mirror of the redo order.
    for( int i = targets.size() - 1; i >= 0; --i ) {
        // Index-PATH from the root mixer, so a track inside a folder can be
        // removed too. `indexOfChildObject()` sees the mixer's DIRECT children
        // only and answered -1 on a nested track, which is how "Remove track"
        // once did nothing at all -- no action, no message, no undo entry.
        const QList<int> path = strackpath::pathOf( mixer, targets.at( i ) );
        if( path.isEmpty() ) continue;      // not in this project's tree
        submit( new SRemoveTrackAction( path ) );
    }
    if( macro ) st->endMacro();

    // Nothing that was removed may stay selected.
    QList<STrack *> keep;
    for( STrack *s : mixer->getSelectedTracks() )
        if( !strackpath::pathOf( mixer, s ).isEmpty() ) keep.append( s );
    mixer->setSelectedTracks( keep );
    return true;
}

bool groupTracks( SStdMixer *mixer, const QList<STrack *> &targets,
                  const Submit &submit )
{
    if( !mixer || targets.isEmpty() || !submit ) return false;

    STrack *first = targets.first();
    const QList<int> tPath = strackpath::pathOf( mixer, first );
    if( tPath.isEmpty() ) return false;
    QList<int> parentPath = tPath;
    const int ti = parentPath.takeLast();   // first target's slot in its parent

    QUndoStack *st = stack();
    if( st ) st->beginMacro( QStringLiteral( "Group track" ) );

    if( parentPath.isEmpty() ) {
        // Top-level block: create the folder directly in its slot.
        submit( new SAddTrackAction( ti ) );
    } else {
        // NESTED. add-track can only append at the MIXER's top level, and
        // SReparentTrackAction refuses a same-container move (that is
        // SMoveTrackAction's job) -- so the folder cannot be born in place and
        // cannot be slid there afterwards if it starts as a sibling. Create it
        // top-level, then move it INTO the parent at the target's slot, which
        // pushes the targets down by one.
        submit( new SAddTrackAction( -1 ) );
        const int folderTop = mixer->getNTracks() - 1;   // the append landed last
        submit( new SReparentTrackAction( QList<int>{ folderTop },
                                          parentPath, ti ) );
    }
    // Resolve the folder BY POINTER from here on: every reparent below shifts
    // the indices its path would otherwise have been spelled with.
    QList<int> folderPath = parentPath; folderPath.append( ti );
    STrack *folder = dynamic_cast<STrack *>(
        strackpath::resolveByPath( mixer, folderPath ) );
    if( folder ) {
        for( STrack *t : targets ) {
            const QList<int> src = strackpath::pathOf( mixer, t );
            if( src.isEmpty() ) continue;
            submit( new SReparentTrackAction(
                src, strackpath::pathOf( mixer, folder ), -1 ) );
        }
    }

    if( st ) st->endMacro();
    return true;
}

namespace {

void ungroupOne( SStdMixer *mixer, STrack *t,
                 const strackgestures::Submit &submit )
{
    if( !t ) return;
    const QList<int> tPath = strackpath::pathOf( mixer, t );
    if( tPath.isEmpty() ) return;
    QList<int> parentPath = tPath;
    const int ti = parentPath.takeLast();   // the folder's slot in ITS parent

    QList<STrack *> kids;
    for( SLink *lk : t->childLinks() )
        if( STrack *k = dynamic_cast<STrack *>( &lk->getSObject() ) )
            kids.append( k );
    if( kids.isEmpty() ) return;

    QUndoStack *st = stack();
    if( st ) st->beginMacro( QStringLiteral( "Ungroup track" ) );
    // Promote each child into the folder's OWN parent -- the mixer when the
    // folder is top-level, the grandparent folder when it is nested (this used
    // to hard-code the mixer, so ungrouping a nested folder flung its children
    // out to the top level). Fill the slots just before the folder so they end
    // up where it was, in order; each insert pushes the folder one further
    // right, which is why insertAt just increments. Actions apply
    // synchronously, so pathOf() re-reads the tree after the previous move.
    int insertAt = ti;
    for( STrack *k : kids ) {
        submit( new SReparentTrackAction( strackpath::pathOf( mixer, k ),
                                          parentPath, insertAt ) );
        ++insertAt;
    }
    // Delete the now-empty folder (undoable: its restore brings it back, then
    // the child reparents undo back into it).
    const QList<int> fPath = strackpath::pathOf( mixer, t );
    if( !fPath.isEmpty() ) submit( new SRemoveTrackAction( fPath ) );
    if( st ) st->endMacro();
}

}  // namespace

bool ungroupTracks( SStdMixer *mixer, const QList<STrack *> &targets,
                    const Submit &submit )
{
    if( !mixer || targets.isEmpty() || !submit ) return false;
    QUndoStack *st = stack();
    const bool macro = targets.size() > 1 && st;
    if( macro ) st->beginMacro( QStringLiteral( "Ungroup tracks" ) );
    for( int i = targets.size() - 1; i >= 0; --i )
        ungroupOne( mixer, targets.at( i ), submit );
    if( macro ) st->endMacro();
    return true;
}

}  // namespace strackgestures
