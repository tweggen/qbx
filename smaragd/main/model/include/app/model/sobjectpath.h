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

// --- Root-qualified paths (proposal 09 D21) ---------------------------------
//
// A project has more than one summing root: the MASTER, plus any number of
// named ARRANGEMENTs (SProject's arrangement registry). An index path alone
// cannot say which of them it addresses -- and index {0} usually exists in all
// of them, so an unqualified path resolved against the wrong root SUCCEEDS and
// edits the wrong tree. That is silent corruption, not a failed action, which
// is why the root travels WITH the path rather than as ambient state.
//
//   "Drums:0,1"   the 2nd child of Drums' 1st lane
//   "0,1"         the same address in the MASTER -- the existing spelling,
//                 unchanged, which is what keeps every project file, every
//                 .qxa case and both goldens byte-identical
//   "Drums:"      the Drums root itself (the empty path)
//
// The separator is ':' because a path is digits and commas and can never
// contain one. An arrangement NAME could, in principle; parsing splits on the
// FIRST colon, so a name containing one is REFUSED by
// SProject::registerArrangement rather than escaped here.
struct QualifiedPath {
    QString    root;   // empty == the master root
    QList<int> idx;
};

inline QualifiedPath parseQualified( const QString &spec )
{
    QualifiedPath q;
    const int colon = spec.indexOf( QLatin1Char(':') );
    if( colon < 0 ) {
        q.idx = stringToPath( spec );
        return q;
    }
    q.root = spec.left( colon );
    q.idx  = stringToPath( spec.mid( colon + 1 ) );
    return q;
}

// Inverse of parseQualified. A master-rooted path writes NO qualifier, so
// nothing that was written before this reads or writes differently.
inline QString qualifiedToString( const QString &root, const QList<int> &idx )
{
    const QString p = pathToString( idx );
    return root.isEmpty() ? p : ( root + QLatin1Char(':') + p );
}

// Parse a possibly-qualified spec, recording its root in `rootInOut` when the
// spec carries one. Actions that address SEVERAL paths call this once per
// attribute and share one root member: the first qualifier wins, which is
// right because an action addressing two roots at once is not an edit, it is
// an extract-arrangement.
inline QList<int> parseInto( QString &rootInOut, const QString &spec )
{
    const QualifiedPath q = parseQualified( spec );
    if( !q.root.isEmpty() && rootInOut.isEmpty() ) rootInOut = q.root;
    return q.idx;
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
