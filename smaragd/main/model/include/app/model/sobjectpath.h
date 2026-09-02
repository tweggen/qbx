#ifndef SOBJECTPATH_H
#define SOBJECTPATH_H

#include <QList>
#include <QString>
#include <QStringList>

#include <limits>

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

// --- SYSTEM-LANE SENTINELS (proposal 45 D9) ---------------------------------
//
// A system lane -- the master, and later a send -- is deliberately not among
// its root's childLinks() (SStdMixer::masterLane() says why that is forced),
// so no non-negative index can reach it. It is addressed by a NEGATIVE step
// instead, which keeps a path a plain QList<int>: every action that stores one
// as a member, every serialized trackPath and every existing call site is
// unchanged, and pathOf() has a representable answer to give.
//
// The alternative -- a separate role field on QualifiedPath -- is cleaner in
// the abstract and was rejected on cost: every action storing a bare
// QList<int> would have to grow a second member.
//
//   -1        the root's master lane
//   -2 - k    its k-th send lane          (reserved; sends are proposal 45 M7)
//   INT_MIN   NOT A PATH -- see stringToPath()
//
// Resolution goes through SObject::systemLaneAt(), so this header needs no
// knowledge of what a mixer is.
static const int SPATH_MASTER  = -1;
static const int SPATH_INVALID = std::numeric_limits<int>::min();

inline int spathSendSentinel( int k )  { return -2 - k; }
inline bool spathIsSystemStep( int i ) { return i < 0 && i != SPATH_INVALID; }

// Descend the path from `root`; returns the SObject it points at, nullptr on
// any out-of-range step. A NEGATIVE step is a system lane (above); an
// unparsable one (SPATH_INVALID) resolves to nothing, which is what makes a
// mistyped "$mastr" fail closed instead of silently addressing track 0.
inline SObject *resolveByPath( SObject *root, const QList<int> &path )
{
    SObject *cur = root;
    for( int idx : path ) {
        if( !cur ) return nullptr;
        if( idx == SPATH_INVALID ) return nullptr;
        if( spathIsSystemStep( idx ) ) {
            cur = cur->systemLaneAt( idx );
            continue;
        }
        SLink *lk = childLinkAt( cur, idx );
        if( !lk ) return nullptr;
        cur = &lk->getSObject();
    }
    return cur;
}

// The SURFACE SPELLING of a system step is "$master" (proposal 45 D9). "$" is
// chosen because it cannot begin a decimal index, so no existing path can
// collide with one.
inline QString pathToString( const QList<int> &p )
{
    QStringList parts;
    for( int v : p ) {
        if( v == SPATH_MASTER )      parts << QStringLiteral( "$master" );
        else if( spathIsSystemStep( v ) )
            // Reserved for sends (M7); spelled by index until the name form
            // exists, so a path is never silently written as something that
            // would read back as an ordinary index.
            parts << QStringLiteral( "$send%1" ).arg( -2 - v );
        else                          parts << QString::number( v );
    }
    return parts.join( "," );
}

// An UNPARSABLE component becomes SPATH_INVALID rather than 0, and `ok` (when
// given) says so.
//
// This is the load-bearing half of D9. QString::toInt() answers 0 for any
// non-numeric text, so before the sentinel existed the spelling "$master"
// resolved silently to THE FIRST USER TRACK -- the "resolved against the wrong
// root SUCCEEDS" corruption class the qualified-path comment below warns
// about, arriving through the other door. Failing closed is the only safe
// answer for an address nobody can check by eye.
inline QList<int> stringToPath( const QString &s, bool *ok = nullptr )
{
    if( ok ) *ok = true;
    QList<int> out;
    const QStringList parts = s.split( ",", Qt::SkipEmptyParts );
    for( const QString &p : parts ) {
        const QString t = p.trimmed();
        if( t == QLatin1String( "$master" ) ) { out << SPATH_MASTER; continue; }
        if( t.startsWith( QLatin1String( "$send" ) ) ) {
            bool n = false;
            const int k = t.mid( 5 ).toInt( &n );
            if( n && k >= 0 ) { out << spathSendSentinel( k ); continue; }
            if( ok ) *ok = false;
            out << SPATH_INVALID;
            continue;
        }
        bool n = false;
        const int v = t.toInt( &n );
        if( !n ) {
            if( ok ) *ok = false;
            out << SPATH_INVALID;
            continue;
        }
        out << v;
    }
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
    // SYSTEM LANES FIRST (proposal 45 D9). A system lane is not among
    // childLinks(), so without this branch pathOf() answers {} for the master
    // lane -- which is ALSO the address of "the root itself". Every track head
    // derives its commit address from pathOf(), so the master's own fader
    // would move and then commit to the mixer, or to nothing at all.
    //
    // The subtree walk below it is what makes a CONDUCTOR lane at {-1,0}
    // reachable; without it a gesture on a conductor row would address the
    // mixer for exactly the same reason.
    if( const int sentinel = cur->systemLaneSentinelOf( target ) ) {
        acc.append( sentinel );
        return true;
    }
    for( int sentinel = SPATH_MASTER; sentinel > -64; --sentinel ) {
        SObject *lane = cur->systemLaneAt( sentinel );
        if( !lane ) break;
        acc.append( sentinel );
        if( lane->isPathContainer() && findPathRec( lane, target, acc ) )
            return true;
        acc.removeLast();
    }

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
