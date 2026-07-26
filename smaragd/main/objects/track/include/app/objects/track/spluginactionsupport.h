#ifndef SPLUGINACTIONSUPPORT_H
#define SPLUGINACTIONSUPPORT_H

#include <QString>

#include "app/model/sproject.h"
#include "app/objects/track/spluginchain.h"
#include "app/objects/track/spluginslot.h"
#include "app/objects/track/strackpath.h"

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
    SObject *root = project->getRootComponent();
    if( !root ) return nullptr;
    STrack *track = dynamic_cast<STrack *>(
        strackpath::resolveByPath( root, strackpath::stringToPath( trackPath ) ) );
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
