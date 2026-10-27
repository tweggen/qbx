#include "app/model/sroutinggraph.h"

#include <QSet>

namespace sroute {

const char *edgeKindName( EdgeKind k )
{
    switch( k ) {
    case EdgeKind::Containment: return "containment";
    case EdgeKind::Send:        return "send";
    case EdgeKind::Aux:         return "aux";
    case EdgeKind::Key:         return "key";
    }
    return "?";
}

void Graph::add( const Edge &e )
{
    out_[e.from].append( e );
    all_.append( e );
}

bool Graph::accept( const Edge &e )
{
    if( wouldCloseCycle( e.from, e.to ) ) return false;
    add( e );
    return true;
}

bool Graph::reaches( Node from, Node to ) const
{
    // Walked on an EDIT or a wiring pass, never per page: the graph is at most
    // (lanes x routing edges).
    QSet<Node> seen;
    QList<Node> work;
    work.append( from );
    while( !work.isEmpty() ) {
        Node cur = work.takeLast();
        if( seen.contains( cur ) ) continue;
        seen.insert( cur );
        if( cur == to ) return true;
        const auto it = out_.constFind( cur );
        if( it == out_.constEnd() ) continue;
        for( const Edge &e : it.value() ) work.append( e.to );
    }
    return false;
}

QList<Edge> acceptInOrder( Graph &g, const QList<Edge> &candidates )
{
    QList<Edge> dropped;
    for( const Edge &e : candidates )
        if( !g.accept( e ) ) dropped.append( e );
    return dropped;
}

}  // namespace sroute
