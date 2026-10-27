#ifndef APP_MODEL_SROUTINGGRAPH_H
#define APP_MODEL_SROUTINGGRAPH_H

#include <QHash>
#include <QList>

/**
 * THE ROUTING GRAPH (proposal 55 D7, built in M0).
 *
 * A lane's output depends on lanes that are not under it. Four edge kinds say
 * which, each pointing from the lane whose output is READ to the lane that
 * reads it:
 *
 *   containment  child lane  -> its folder          (childLinks())
 *   send         source lane -> send lane           (SSendTap, proposal 47 D2)
 *   aux          source track -> return track       (proposal 55 D8, M2)
 *   key          key lane    -> the keyed track     (proposal 55 D8, M4)
 *
 * The UNION must be acyclic as handed to the scheduler (55 inv. 3). Cycles are
 * refused at the verb and broken at the mixer's wiring pass -- 47 D6's two
 * layers -- and both layers ask THIS type, so they cannot disagree about what
 * a cycle is. Before M0 each layer had its own send-only walk
 * (ssendtapactions.cpp `reaches`, sstdmixer.cpp `sendReaches`).
 *
 * A pure graph over opaque node identities: it never dereferences a node, so
 * the model passes SObject pointers and a unit test passes anything distinct.
 * It lives in app/model, the lowest slice both the verbs and the pass can see.
 */
namespace sroute {

enum class EdgeKind { Containment, Send, Aux, Key };

/// Stable lowercase spelling for logs and describe strings.
const char *edgeKindName( EdgeKind k );

using Node = const void *;

struct Edge {
    Node     from = nullptr;   ///< the lane whose output is read
    Node     to   = nullptr;   ///< the lane that reads it
    EdgeKind kind = EdgeKind::Send;
};

class Graph {
public:
    /// Add `e` with no check. For the MODEL's own graph, which a hand-edited
    /// file may leave cyclic: a verb asks it, and must see every edge the
    /// user has, cycle or not.
    void add( const Edge &e );

    /// THE PASS'S RULE: add `e` only when it does not close a cycle among the
    /// edges already in the graph (a self edge is a cycle of one). Returns
    /// whether it was added. Fed in a fixed order, this is deterministic and
    /// drops exactly one edge of each cycle -- asking the FULL graph instead
    /// would find every edge of the cycle cyclic and drop them all.
    bool accept( const Edge &e );

    /// True when `from` reaches `to` along edges of any kind (from == to
    /// counts). Walks a cyclic graph safely.
    bool reaches( Node from, Node to ) const;

    /// True when an edge from -> to would close a cycle: what a verb refuses.
    bool wouldCloseCycle( Node from, Node to ) const
    {
        return from == to || reaches( to, from );
    }

    /// The edges leaving `n`, in the order they were added.
    QList<Edge> edgesFrom( Node n ) const { return out_.value( n ); }

    /// Every edge, in the order added.
    const QList<Edge> &edges() const { return all_; }

private:
    QHash<Node, QList<Edge> > out_;
    QList<Edge> all_;
};

/// Feed `candidates` to `g.accept()` in order; returns the DROPPED edges, in
/// the order they were dropped. The pass's whole algorithm, spelled once so the
/// unit test drives exactly what the pass runs.
QList<Edge> acceptInOrder( Graph &g, const QList<Edge> &candidates );

}  // namespace sroute

#endif
