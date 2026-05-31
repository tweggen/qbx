#ifndef _TWNEGOTIATOR_H_
#define _TWNEGOTIATOR_H_

class tw303aEnvironment;
class twComponent;

// Resolves a single sample format per wire for the connected subgraph feeding a
// sink (proposal 04 §3b). It builds a finite candidate-rate domain D, runs the
// monotone narrowCaps relations to an arc-consistency fixpoint, resolves any
// residual freedom by preference (project rate first), and commits one format
// per port via twComponent::commitFormats — all off the realtime path.
//
// Today every source produces at the environment rate, so the graph is uniform
// and always feasible; the speaker's own resampler bridges graph-rate vs.
// device-rate. Infeasible wires (which would require a fixed-rate source at a
// different rate) are detected and logged; live insertion of a resampler node
// to heal them is the deferred open fork (see proposal 04, Open items).
class twNegotiator
{
public:
    explicit twNegotiator( tw303aEnvironment &env );

    // Returns true if a consistent format assignment was committed for the
    // subgraph feeding `target`; false if some wire was infeasible. The caller
    // may treat the result as advisory.
    bool negotiate( twComponent *target );

private:
    tw303aEnvironment &env_;
};

#endif
