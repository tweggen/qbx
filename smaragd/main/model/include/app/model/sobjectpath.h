#ifndef SOBJECTPATH_H
#define SOBJECTPATH_H

#include <QList>
#include <QString>
#include <QStringList>

#include "app/model/sobject.h"
#include "app/model/slink.h"

// Generic index-path addressing over the SObject tree (proposal 14, Phase 6:
// split out of strackpath.h so path resolution needs no track knowledge).
// [] is the root, {2} its 3rd child, {2,1} the 2nd child of that. Containers
// expose ordered SLink children through childAt()/childCount()/childLinks().
// Kept in namespace strackpath so existing call sites are unchanged; the
// track-flavored helpers (pathOf, isSelfOrDescendant) stay in
// app/objects/track/strackpath.h, which includes this header.
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

inline bool findPathRec( SObject *cur, SObject *target, QList<int> &acc )
{
    int i = 0;
    for( SLink *lk : cur->childLinks() ) {
        SObject *child = &lk->getSObject();
        acc.append( i );
        if( child == target ) return true;
        // Only path containers (track lanes) can hold further placements;
        // don't dive into clips (see SObject::isPathContainer).
        if( child->isPathContainer() && findPathRec( child, target, acc ) ) {
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

} // namespace strackpath

#endif // SOBJECTPATH_H
