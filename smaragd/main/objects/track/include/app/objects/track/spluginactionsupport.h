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
    const strackpath::QualifiedPath q = strackpath::parseQualified( trackPath );
    SObject *root = splacements::rootNamed( project, q.root );
    if( !root ) return nullptr;
    STrack *track = dynamic_cast<STrack *>(
        strackpath::resolveByPath( root, q.idx ) );
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
