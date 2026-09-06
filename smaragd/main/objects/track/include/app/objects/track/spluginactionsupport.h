#ifndef SPLUGINACTIONSUPPORT_H
#define SPLUGINACTIONSUPPORT_H

#include <QString>

#include "app/model/sproject.h"
#include "app/objects/track/spluginchain.h"
#include "app/objects/track/spluginslot.h"
#include "app/objects/track/strackpath.h"
#include "app/model/splacements.h"

// Shared resolution for the five plugin actions (proposal 08 M3/M5).
//
// insert-plugin and remove-plugin each carried their own copy of "parse the
// track path, cast to STrack, ask for the chain". M5 adds three more actions
// and the FX strip needs the reverse direction, so the rule lives in ONE place:
// a fifth and sixth copy is how the six would drift.
//
// The path is the project-wide convention (app/model/sobjectpath.h): a
// comma-separated index path from the root mixer, so "0" is the first track and
// "0,2" is that track's third child lane.
namespace spluginaction {

inline SPluginChain *chainFor( SProject *project, const QString &trackPath )
{
    if( !project ) return nullptr;
    // A qualified trackPath ("Drums:0") names its own root (proposal 09 D21);
    // a bare one means the master, which is what every caller written before
    // arrangements existed passes.
    //
    // THROUGH laneBySpec SINCE PROPOSAL 45 M7, which is the whole of AC7.2:
    // it does the same peel-root-then-resolve, and additionally understands
    // `$send:<name>`. That name cannot be resolved by the pure string parser
    // (it maps to a sentinel only against a particular root, and readXml has
    // no project), so this is the seam where it becomes a path -- and putting
    // it HERE is what makes reorder-plugin, set-plugin-bypass and
    // set-plugin-param reach a send lane with no edit of their own.
    QString root;
    STrack *track = dynamic_cast<STrack *>(
        splacements::laneBySpec( project, trackPath, root ) );
    return track ? track->getPluginChain() : nullptr;
}

inline SPluginSlot *slotFor( SProject *project, const QString &trackPath,
                             int slotIndex )
{
    SPluginChain *chain = chainFor( project, trackPath );
    return chain ? chain->getSlotAt( slotIndex ) : nullptr;
}

}  // namespace spluginaction

#endif  // SPLUGINACTIONSUPPORT_H
