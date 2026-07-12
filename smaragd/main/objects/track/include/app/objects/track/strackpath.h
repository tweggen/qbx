#ifndef STRACKPATH_H
#define STRACKPATH_H

#include <QList>
#include <QString>
#include <QStringList>

#include "app/model/sobjectpath.h"   // generic resolveByPath / path<->string
#include "app/objects/track/strack.h"

// Shared helpers for the track-tree actions. Tracks and containers are
// addressed by an index-path from the root mixer: [] is the root, {2} its 3rd
// child, {2,1} the 2nd child of that. Containers (mixer or track) expose their
// ordered SLink children through SObject's childAt()/childCount()/childLinks(),
// so these helpers never touch QObject::children() directly.
namespace strackpath {



// True if `candidate` is `ancestor` itself or somewhere below it — the cycle
// guard for reparenting.
inline bool isSelfOrDescendant( SObject *candidate, STrack *ancestor )
{
    if( candidate == static_cast<SObject*>( ancestor ) ) return true;
    for( SLink *lk : ancestor->childLinks() ) {
        STrack *ct = dynamic_cast<STrack*>( &lk->getSObject() );
        if( ct && isSelfOrDescendant( candidate, ct ) ) return true;
    }
    return false;
}


} // namespace strackpath

#endif // STRACKPATH_H
