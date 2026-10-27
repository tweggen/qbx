// THE ROUTING GRAPH (proposal 55 D7, M0 AC0.3).
//
// One walk decides what a cycle is for every verb that adds a routing edge and
// for the mixer pass that breaks the cycles a file can still carry. Before M0
// there were two send-only copies of it; M2 and M4 add the aux and key edges
// to THIS walk, so it is gated here over synthetic graphs carrying all four
// edge kinds before any model produces an aux or a key edge.
//
// WHY A UNIT TEST. The cycle shapes that need containment -- a track keyed
// from its own folder, a return placed inside its own source -- cannot be
// built by any verb before M2/M4, and the .qxa suite can only reach the send
// shapes (send_cycle_broken, send_tap_model). The graph is a pure function of
// its edges, which is exactly what a unit test is for.

#include "app/model/sroutinggraph.h"

#include <cstdio>
#include <string>

using sroute::Edge;
using sroute::EdgeKind;
using sroute::Graph;
using sroute::Node;

static int gFailures = 0;

static void check( bool cond, const std::string &what )
{
    std::printf( "%s %s\n", cond ? "PASS" : "FAIL", what.c_str() );
    if( !cond ) ++gFailures;
}

// Distinct addresses stand in for lanes: the graph never dereferences a node.
static char gLanes[16];
static Node lane( int i ) { return &gLanes[i]; }

static Edge E( int from, int to, EdgeKind k ) { return Edge{ lane( from ), lane( to ), k }; }

static bool sameEdge( const Edge &a, const Edge &b )
{
    return a.from == b.from && a.to == b.to && a.kind == b.kind;
}

// Lane names, for readable shapes.
enum { Master = 0, Folder, Bass, Kick, Source, Return, SendA, SendB, Other };

// Every cycle shape proposal 55 D7 names, built the way the pass sees it:
// the existing edges ACCEPTED first, then the closing edge, which accept()
// must refuse and wouldCloseCycle() must predict.
static void refusedShape( const char *name, const std::initializer_list<Edge> &existing,
                          const Edge &closing )
{
    Graph g;
    bool allIn = true;
    for( const Edge &e : existing ) allIn = g.accept( e ) && allIn;
    check( allIn, std::string( name ) + ": the existing edges are acyclic and accepted" );
    check( g.wouldCloseCycle( closing.from, closing.to ),
           std::string( name ) + ": wouldCloseCycle predicts the cycle" );
    const std::size_t before = (std::size_t)g.edges().size();
    check( !g.accept( closing ), std::string( name ) + ": the closing edge is REFUSED" );
    check( (std::size_t)g.edges().size() == before,
           std::string( name ) + ": a refused edge is not added" );
}

static void acceptedShape( const char *name, const std::initializer_list<Edge> &existing,
                           const Edge &next )
{
    Graph g;
    for( const Edge &e : existing ) g.accept( e );
    check( !g.wouldCloseCycle( next.from, next.to ),
           std::string( name ) + ": wouldCloseCycle says no cycle" );
    check( g.accept( next ), std::string( name ) + ": the edge is ACCEPTED" );
}

int main()
{
    std::printf( "=== sroute::Graph: the cycle shapes of proposal 55 D7 ===\n" );

    // A self edge is a cycle of one, of every kind.
    refusedShape( "key from itself", {}, E( Bass, Bass, EdgeKind::Key ) );
    refusedShape( "send to itself", {}, E( SendA, SendA, EdgeKind::Send ) );
    refusedShape( "return of itself", {}, E( Return, Return, EdgeKind::Aux ) );

    // THE CONTAINMENT-ONLY SHAPES: no routing edge closes them on its own.
    // Bass sits in Folder (Bass -> Folder); keying Bass FROM Folder reads a
    // sum that already contains Bass's own output.
    refusedShape( "key from own folder",
                  { E( Bass, Folder, EdgeKind::Containment ) },
                  E( Folder, Bass, EdgeKind::Key ) );
    // ...and two folders up: containment is transitive.
    refusedShape( "key from own grandparent folder",
                  { E( Bass, Folder, EdgeKind::Containment ),
                    E( Folder, Other, EdgeKind::Containment ) },
                  E( Other, Bass, EdgeKind::Key ) );
    // A return placed INSIDE its own source track (Source is a folder holding
    // Return): Source's sum contains Return, and Return reads Source's aux.
    refusedShape( "return inside its own source",
                  { E( Return, Source, EdgeKind::Containment ) },
                  E( Source, Return, EdgeKind::Aux ) );
    // The same, closed from the other side: the aux edge exists and the
    // return is then moved into its source.
    refusedShape( "return moved into its source",
                  { E( Source, Return, EdgeKind::Aux ) },
                  E( Return, Source, EdgeKind::Containment ) );

    // The routing-only shapes (AC4.5's list).
    refusedShape( "return's source keyed from the return",
                  { E( Source, Return, EdgeKind::Aux ) },
                  E( Return, Source, EdgeKind::Key ) );
    refusedShape( "send loop",
                  { E( SendA, SendB, EdgeKind::Send ) },
                  E( SendB, SendA, EdgeKind::Send ) );
    refusedShape( "send loop closed by a key",
                  { E( Bass, SendA, EdgeKind::Send ) },
                  E( SendA, Bass, EdgeKind::Key ) );
    refusedShape( "aux, send, key around three lanes",
                  { E( Source, Return, EdgeKind::Aux ),
                    E( Return, SendA, EdgeKind::Send ) },
                  E( SendA, Source, EdgeKind::Key ) );
    refusedShape( "all four kinds in one cycle",
                  { E( Kick, Folder, EdgeKind::Containment ),
                    E( Folder, SendA, EdgeKind::Send ),
                    E( SendA, Return, EdgeKind::Key ) },
                  E( Return, Kick, EdgeKind::Aux ) );

    // NOT cycles, which a walk that over-refuses would get wrong.
    acceptedShape( "key from a sibling in the same folder",
                   { E( Bass, Folder, EdgeKind::Containment ),
                     E( Kick, Folder, EdgeKind::Containment ) },
                   E( Kick, Bass, EdgeKind::Key ) );
    acceptedShape( "a return beside its source, keying a third track",
                   { E( Source, Return, EdgeKind::Aux ) },
                   E( Return, Bass, EdgeKind::Key ) );
    acceptedShape( "a diamond (two paths to one lane) is not a cycle",
                   { E( Source, Return, EdgeKind::Aux ),
                     E( Source, SendA, EdgeKind::Send ),
                     E( Return, SendB, EdgeKind::Send ) },
                   E( SendA, SendB, EdgeKind::Send ) );

    std::printf( "=== the deterministic drop order ===\n" );
    {
        // Two independent cycles in one candidate list, in file order. The
        // pass must drop exactly the edge that CLOSES each one -- the later of
        // each pair -- and keep the rest, every time.
        const QList<Edge> candidates = {
            E( Bass, Folder, EdgeKind::Containment ),   // 0  accepted
            E( SendA, SendB, EdgeKind::Send ),          // 1  accepted
            E( Folder, Bass, EdgeKind::Key ),           // 2  closes 0 -> DROPPED
            E( Source, Return, EdgeKind::Aux ),         // 3  accepted
            E( SendB, SendA, EdgeKind::Send ),          // 4  closes 1 -> DROPPED
            E( Return, Source, EdgeKind::Key ),         // 5  closes 3 -> DROPPED
            E( Return, Kick, EdgeKind::Key ),           // 6  accepted
        };
        Graph g;
        const QList<Edge> dropped = sroute::acceptInOrder( g, candidates );
        check( dropped.size() == 3, "exactly three edges dropped" );
        check( dropped.size() == 3 && sameEdge( dropped[0], candidates[2] ),
               "first drop: the key from Bass's own folder" );
        check( dropped.size() == 3 && sameEdge( dropped[1], candidates[4] ),
               "second drop: the send closing SendA -> SendB" );
        check( dropped.size() == 3 && sameEdge( dropped[2], candidates[5] ),
               "third drop: the key closing the aux edge" );
        check( g.edges().size() == 4, "the four acyclic edges are kept" );
        bool keptInOrder = g.edges().size() == 4 &&
                           sameEdge( g.edges()[0], candidates[0] ) &&
                           sameEdge( g.edges()[1], candidates[1] ) &&
                           sameEdge( g.edges()[2], candidates[3] ) &&
                           sameEdge( g.edges()[3], candidates[6] );
        check( keptInOrder, "kept edges stay in candidate order" );

        // Repeating the pass gives the same answer: the break depends on the
        // order of the candidates and on nothing else.
        Graph g2;
        const QList<Edge> again = sroute::acceptInOrder( g2, candidates );
        bool same = again.size() == dropped.size();
        for( int i = 0; same && i < again.size(); ++i )
            same = sameEdge( again[i], dropped[i] );
        check( same, "a second pass over the same candidates drops the same edges" );

        // ...and the ORDER is what decides: reversed, the other edge of each
        // pair is the one that closes the cycle. This is what makes "lane
        // order = file order" the rule rather than an accident.
        QList<Edge> reversed;
        for( int i = candidates.size() - 1; i >= 0; --i ) reversed.append( candidates[i] );
        Graph g3;
        const QList<Edge> droppedRev = sroute::acceptInOrder( g3, reversed );
        bool flipped = droppedRev.size() == 3 &&
                       sameEdge( droppedRev[0], candidates[3] ) &&
                       sameEdge( droppedRev[1], candidates[1] ) &&
                       sameEdge( droppedRev[2], candidates[0] );
        check( flipped, "reversed order drops the OTHER edge of each cycle" );
    }

    std::printf( "=== the model graph may already be cyclic ===\n" );
    {
        // A hand-edited file can carry a cycle; the verb's graph keeps every
        // edge (add, not accept) and must still answer without looping.
        Graph g;
        g.add( E( SendA, SendB, EdgeKind::Send ) );
        g.add( E( SendB, SendA, EdgeKind::Send ) );
        check( g.reaches( lane( SendA ), lane( SendB ) ), "reaches across a cycle" );
        check( !g.reaches( lane( SendA ), lane( Bass ) ),
               "an unreachable lane stays unreachable in a cyclic graph" );
        check( g.wouldCloseCycle( lane( Bass ), lane( Bass ) ),
               "a self edge is a cycle even with no edges at all" );
        check( g.edgesFrom( lane( SendA ) ).size() == 1, "edgesFrom lists one edge" );
    }

    check( std::string( sroute::edgeKindName( EdgeKind::Containment ) ) == "containment" &&
           std::string( sroute::edgeKindName( EdgeKind::Send ) ) == "send" &&
           std::string( sroute::edgeKindName( EdgeKind::Aux ) ) == "aux" &&
           std::string( sroute::edgeKindName( EdgeKind::Key ) ) == "key",
           "edge kind spellings" );

    std::printf( "%s: %d failure(s)\n", gFailures ? "FAILED" : "OK", gFailures );
    return gFailures ? 1 : 0;
}
