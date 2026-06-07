#ifndef STRACKPATH_H
#define STRACKPATH_H

#include <QList>
#include <QString>
#include <QStringList>

#include "sobject.h"
#include "slink.h"
#include "strack.h"

// Shared helpers for the track-tree actions. Tracks and containers are
// addressed by an index-path from the root mixer: [] is the root, {2} its 3rd
// child, {2,1} the 2nd child of that. Containers (mixer or track) expose their
// ordered SLink children through SObject's childAt()/childCount()/childLinks(),
// so these helpers never touch QObject::children() directly.
namespace strackpath {

inline SLink *childLinkAt( SObject *container, int idx )
{
    return container ? container->childAt( idx ) : nullptr;
}

// Descend the path from `root`; returns the SObject it points at, nullptr on
// any out-of-range step.
inline SObject *resolveByPath( SObject *root, const QList<int> &path )
{
    SObject *cur = root;
    for( int idx : path ) {
        SLink *lk = childLinkAt( cur, idx );
        if( !lk ) return nullptr;
        cur = &lk->getSObject();
    }
    return cur;
}

inline bool findPathRec( SObject *cur, SObject *target, QList<int> &acc )
{
    int i = 0;
    for( SLink *lk : cur->childLinks() ) {
        SObject *child = &lk->getSObject();
        acc.append( i );
        if( child == target ) return true;
        // Only track containers can hold further tracks; don't dive into clips.
        if( dynamic_cast<STrack*>( child ) && findPathRec( child, target, acc ) ) {
            return true;
        }
        acc.removeLast();
        ++i;
    }
    return false;
}

// Index-path from `root` to `target`. Empty list == root itself.
inline QList<int> pathOf( SObject *root, SObject *target )
{
    QList<int> acc;
    if( root == target ) return acc;
    findPathRec( root, target, acc );
    return acc;
}

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

inline QString pathToString( const QList<int> &p )
{
    QStringList parts;
    for( int v : p ) parts << QString::number( v );
    return parts.join( "," );
}

inline QList<int> stringToPath( const QString &s )
{
    QList<int> out;
    const QStringList parts = s.split( ",", Qt::SkipEmptyParts );
    for( const QString &p : parts ) out << p.toInt();
    return out;
}

} // namespace strackpath

#endif // STRACKPATH_H
