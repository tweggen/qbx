#include "app/objects/track/srecordmode.h"
#include "app/objects/track/strack.h"
#include "app/model/sprojectprops.h"
#include "app/model/sproject.h"
#include "app/model/sobject.h"
#include "app/model/slink.h"

namespace srecmode {

namespace {

// THE WALK IS DOWNWARD FROM THE ROOT, not upward from the lane, because
// SObject has no parent pointer: the tree is SObject -> SLink -> SObject and
// only the links point down. ssolo::underSoloedAncestorRec has exactly this
// shape and for exactly this reason; this follows it rather than inventing a
// second way to find an ancestor.
//
// `innermost` carries the nearest FOLDER-track mode seen on the way down, so
// a folder inside a folder overrides the outer one by simply overwriting it.
//
// Returns true once the lane has been found under this subtree, with `found`
// set to the mode that applies. A lane placed under more than one parent
// resolves through the FIRST placement reached, which is the same tie-break
// the audibility walk takes; a lane with two parents disagreeing about the
// record mode is not a shape the model can express today (a track has one
// placement) and is left for whoever makes it expressible.
bool resolveRec( const SObject *cur, const SObject *lane,
                 const QString &innermost, QString *found )
{
    if( !cur ) return false;
    for( SLink *lk : const_cast<SObject *>( cur )->childLinks() ) {
        if( !lk ) continue;
        const SObject *child = &lk->getSObject();
        if( child == lane ) {
            *found = innermost;
            return true;
        }
        if( !child->isLane() ) continue;
        QString next = innermost;
        if( const STrack *t = dynamic_cast<const STrack *>( child ) ) {
            // Only a FOLDER track acts as a group (plan 53 D2, decided (a)).
            // Without the hasChildTracks() test a plain parent lane would
            // behave as one.
            if( t->hasChildTracks() && !t->getRecordMode().isEmpty() )
                next = t->getRecordMode();
        }
        if( resolveRec( child, lane, next, found ) ) return true;
    }
    return false;
}

} // namespace

QString projectMode( const SProject *project )
{
    if( !project ) return QString::fromLatin1( SProjectProps::recordModeDefault() );
    const QString m =
        const_cast<SProject *>( project )
            ->prop( SProjectProps::RecordMode,
                    QString::fromLatin1(
                        SProjectProps::recordModeDefault() ) ).toString();
    // A project file can carry anything; an unknown value reads as the default
    // rather than propagating a mode no planner knows.
    return SProjectProps::isRecordMode( m )
               ? m
               : QString::fromLatin1( SProjectProps::recordModeDefault() );
}

QString resolveMode( const SObject *root, const SObject *lane,
                     const SProject *project )
{
    const QString projMode = projectMode( project );
    if( !lane ) return projMode;

    // THE LANE'S OWN ATTRIBUTE WINS over everything, and is the only level that
    // does not need the walk at all.
    if( const STrack *t = dynamic_cast<const STrack *>( lane ) ) {
        if( !t->getRecordMode().isEmpty() ) return t->getRecordMode();
    }
    // Then the nearest ancestor folder track, found by descending from the
    // root. A null root (or a lane that is not under it) leaves the project's.
    QString found;
    if( root && resolveRec( root, lane, QString(), &found )
        && !found.isEmpty() ) {
        return found;
    }
    return projMode;
}

} // namespace srecmode
