#include "twnegotiator.h"

#include "tw303aenv.h"
#include "twcomponent.h"
#include "twsyslog.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <map>
#include <set>
#include <vector>

namespace {

using Rates = std::vector<std::uint32_t>;

Rates sortedUnique( Rates v )
{
    std::sort( v.begin(), v.end() );
    v.erase( std::unique( v.begin(), v.end() ), v.end() );
    return v;
}

Rates intersect( const Rates &a, const Rates &b )
{
    Rates out;
    std::set_intersection( a.begin(), a.end(), b.begin(), b.end(),
                           std::back_inserter( out ) );
    return out;
}

// A directed wire: producer output `outIdx` feeds consumer input `inIdx`.
struct Wire {
    twComponent *producer;
    idx_t        outIdx;
    twComponent *consumer;
    idx_t        inIdx;
};

}  // namespace

twNegotiator::twNegotiator( tw303aEnvironment &env )
    : env_( env )
{
}

bool twNegotiator::negotiate( twComponent *target )
{
    return negotiate( target, {} );
}

bool twNegotiator::negotiate( twComponent *target,
                              const std::vector<std::uint32_t> &extraRates )
{
    if( !target ) return true;

    // 1. Discover the upstream subgraph (BFS over input plugs).
    std::vector<twComponent *> comps;
    std::set<twComponent *>    seen;
    std::vector<Wire>          wires;

    std::vector<twComponent *> stack{ target };
    seen.insert( target );
    while( !stack.empty() ) {
        twComponent *c = stack.back();
        stack.pop_back();
        comps.push_back( c );
        for( idx_t i = 0; i < c->getNInputs(); ++i ) {
            twLatchOutput *plug = c->getInputPlug( i );
            if( !plug ) continue;
            twLatch     &lat = plug->getParentLatch();
            twComponent &p   = lat.getComponent();
            wires.push_back( Wire{ &p, lat.getIndex(), c, i } );
            if( !seen.count( &p ) ) {
                seen.insert( &p );
                stack.push_back( &p );
            }
        }
    }

    // 2. Candidate domain D = configured standard rates ∪ the project rate ∪
    //    rates the device advertises (so a device-native rate can win and avoid
    //    resampling).
    Rates D = env_.candidateRates();
    D.push_back( (std::uint32_t) env_.getSRate() );
    D.insert( D.end(), extraRates.begin(), extraRates.end() );
    D = sortedUnique( D );

    // 3. Seed per-component port domains; expand "any" (empty) to D, otherwise
    //    intersect the declared caps with D.
    auto expand = [&]( twFormatCaps caps ) -> Rates {
        if( caps.rates.empty() ) return D;
        return intersect( sortedUnique( caps.rates ), D );
    };
    std::map<twComponent *, twPortDomains> dom;
    for( twComponent *c : comps ) {
        twPortDomains pd;
        for( idx_t i = 0; i < c->getNInputs();  ++i ) {
            twFormatCaps caps; caps.rates = expand( c->getInputCaps( i ) );
            pd.in.push_back( caps );
        }
        for( idx_t j = 0; j < c->getNOutputs(); ++j ) {
            twFormatCaps caps; caps.rates = expand( c->getOutputCaps( j ) );
            pd.out.push_back( caps );
        }
        dom[c] = std::move( pd );
    }

    // 4. Arc-consistency fixpoint: node coupling (narrowCaps) + wire equality.
    //    Monotone narrowing over the finite domain D terminates in bounded
    //    steps; the iteration cap is only a backstop against a non-monotone bug.
    const int maxIters =
        (int) comps.size() * (int) ( D.size() + 1 ) + (int) wires.size() + 4;
    int  iter    = 0;
    bool changed = true;
    while( changed ) {
        if( ++iter > maxIters ) {
            syslog( LOG_ERR,
                    "twNegotiator: fixpoint did not settle in %d iterations "
                    "(non-monotone narrowCaps?)", maxIters );
            break;
        }
        changed = false;

        for( twComponent *c : comps )
            if( c->narrowCaps( dom[c] ) ) changed = true;

        for( const Wire &w : wires ) {
            Rates &po = dom[w.producer].out[w.outIdx].rates;
            Rates &pi = dom[w.consumer].in[w.inIdx].rates;
            Rates common = intersect( sortedUnique( po ), sortedUnique( pi ) );
            if( sortedUnique( po ) != common ) { po = common; changed = true; }
            if( sortedUnique( pi ) != common ) { pi = common; changed = true; }
        }
    }

    // 5. Detect infeasible wires (empty domain after the fixpoint).
    bool feasible = true;
    for( const Wire &w : wires ) {
        if( dom[w.producer].out[w.outIdx].rates.empty() ) {
            feasible = false;
            syslog( LOG_ERR,
                    "twNegotiator: no common rate on wire %p[out %d] -> "
                    "%p[in %d]. Automatic resampler insertion is not yet "
                    "enabled (open fork); the speaker resampler will bridge "
                    "the sink instead.",
                    (void *) w.producer, (int) w.outIdx,
                    (void *) w.consumer, (int) w.inIdx );
        }
    }

    // 6. Resolve residual freedom: prefer the project rate, else the highest
    //    surviving rate; fall back to the project rate for an empty domain.
    const std::uint32_t projRate = (std::uint32_t) env_.getSRate();
    auto pick = [&]( const Rates &r ) -> std::uint32_t {
        if( r.empty() ) return projRate;
        if( std::find( r.begin(), r.end(), projRate ) != r.end() ) return projRate;
        return *std::max_element( r.begin(), r.end() );
    };

    // 7. Commit one format per port (mono Float32 at the chosen rate).
    for( twComponent *c : comps ) {
        twPortDomains &pd = dom[c];
        std::vector<twFormat> ins, outs;
        ins.reserve( pd.in.size() );
        outs.reserve( pd.out.size() );
        for( const twFormatCaps &caps : pd.in )
            ins.push_back( twCanonicalFormat( pick( caps.rates ) ) );
        for( const twFormatCaps &caps : pd.out )
            outs.push_back( twCanonicalFormat( pick( caps.rates ) ) );
        c->commitFormats( ins.data(),  (idx_t) ins.size(),
                          outs.data(), (idx_t) outs.size() );
    }

    syslog( LOG_INFO,
            "twNegotiator: settled %zu component(s), %zu wire(s) at %u Hz%s",
            comps.size(), wires.size(), (unsigned) projRate,
            feasible ? "" : " (with infeasible wires — see above)" );
    return feasible;
}
