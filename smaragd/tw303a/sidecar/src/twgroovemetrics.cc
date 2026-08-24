#include "tw/sidecar/twgroovemetrics.h"

#include <algorithm>
#include <cmath>

/*
 * Proposal 40 M3b -- read-side metric derivation. See the header for the
 * series contract. Everything here is O(nHops + nEvents) per series via
 * two-pointer sliding windows over the (ascending) event list; the res
 * records are indexed directly.
 */

namespace {

const float kNoData = -1.0f;

/** Median of a SORTED vector (empty -> 0). */
double medianSorted( const std::vector<double> &v )
{
    if( v.empty() ) return 0.0;
    const size_t n = v.size();
    return ( n & 1 ) ? v[n / 2] : 0.5 * ( v[n / 2 - 1] + v[n / 2] );
}

/** Median of an unsorted scratch vector (mutates it). */
double medianOf( std::vector<double> &v )
{
    std::sort( v.begin(), v.end() );
    return medianSorted( v );
}

/** sigma (ms) -> [0,1]: <= jnd is 1.0 (sub-JND jitter is not
 * discriminable and must never be penalized -- section 2.3), >= feelBand
 * is 0.0, linear between. */
double mapSigma( double sigmaMs, const twGrooveReadParams &p )
{
    if( sigmaMs <= p.jndFloorMs ) return 1.0;
    if( p.feelBandMs <= p.jndFloorMs ) return 0.0;
    const double t = ( sigmaMs - p.jndFloorMs ) / ( p.feelBandMs - p.jndFloorMs );
    return std::max( 0.0, 1.0 - t );
}

/** A half-open frame window centered on an aspect hop. */
struct HopWindow {
    double lo = 0.0;
    double hi = 0.0;
};

HopWindow windowAt( uint64_t hop, uint32_t hopFrames, double windowSec, uint32_t rate )
{
    const double center = ( (double)hop + 0.5 ) * (double)hopFrames;
    const double half   = 0.5 * windowSec * (double)rate;
    return { center - half, center + half };
}

/**
 * Sliding two-pointer cursor over the ascending event list: for each hop's
 * window it yields [begin, end) index bounds. The window centers are
 * monotone in hop, so both pointers only ever advance.
 */
struct WindowCursor {
    const std::vector<twGrooveEvRecord> &ev;
    size_t begin = 0;
    size_t end   = 0;

    explicit WindowCursor( const std::vector<twGrooveEvRecord> &e ) : ev( e ) {}

    void advanceTo( const HopWindow &w )
    {
        while( begin < ev.size() && (double)ev[begin].pos < w.lo ) begin++;
        if( end < begin ) end = begin;
        while( end < ev.size() && (double)ev[end].pos < w.hi ) end++;
    }
};

} // namespace

namespace {

/** Inclusive-window mean via a prefix sum: mean of v[lo..hi]. */
struct PrefixMean {
    std::vector<double> pre;   // pre[i] = sum v[0..i-1]
    explicit PrefixMean( const std::vector<double> &v )
    {
        pre.assign( v.size() + 1, 0.0 );
        for( size_t i = 0; i < v.size(); i++ ) pre[i + 1] = pre[i] + v[i];
    }
    double sum( long lo, long hi ) const   // clamped, inclusive
    {
        const long n = (long)pre.size() - 1;
        if( n <= 0 ) return 0.0;
        lo = std::max( 0L, lo );
        hi = std::min( n - 1, hi );
        if( hi < lo ) return 0.0;
        return pre[(size_t)hi + 1] - pre[(size_t)lo];
    }
    double mean( long lo, long hi ) const
    {
        const long n = (long)pre.size() - 1;
        const long a = std::max( 0L, lo ), b = std::min( n - 1, hi );
        if( b < a ) return 0.0;
        return sum( a, b ) / (double)( b - a + 1 );
    }
};

} // namespace

std::vector<twGrooveMetricSeries> twGrooveDeriveMetrics(
    const std::vector<twGrooveResRecord> &res,
    const std::vector<twGrooveEvRecord>  &ev,
    uint32_t hopFrames, uint32_t rate,
    const std::vector<std::string> &unitNames,
    const twGrooveReadParams &params )
{
    return twGrooveDeriveMetrics( res, ev, std::vector<twGrooveDynRecord>{},
                                  hopFrames, rate, unitNames, params );
}

std::vector<twGrooveMetricSeries> twGrooveDeriveMetrics(
    const std::vector<twGrooveResRecord> &res,
    const std::vector<twGrooveEvRecord>  &ev,
    const std::vector<twGrooveDynRecord> &dyn,
    uint32_t hopFrames, uint32_t rate,
    const std::vector<std::string> &unitNames,
    const twGrooveReadParams &params )
{
    std::vector<twGrooveMetricSeries> out;
    if( res.empty() || hopFrames == 0 || rate == 0 ) return out;

    const size_t   nHops  = res.size();
    const uint32_t nUnits = res[0].unitPower.empty() ? 0 : (uint32_t)res[0].unitPower.size();

    // --- compliance: the existing scalar, verbatim (the control row) ------
    {
        twGrooveMetricSeries s;
        s.id    = "compliance";
        s.label = "Compliance (shipped)";
        s.value.reserve( nHops );
        for( const twGrooveResRecord &r : res ) s.value.push_back( r.compliance );
        out.push_back( std::move( s ) );
    }

    // --- power:<unit>: per-unit normalized resonance power ----------------
    for( uint32_t u = 0; u < nUnits; u++ ) {
        twGrooveMetricSeries s;
        const std::string name =
            u < unitNames.size() && !unitNames[u].empty() ? unitNames[u]
                                                          : "unit" + std::to_string( u );
        s.id    = "power:" + name;
        s.label = "Power " + name;
        s.value.reserve( nHops );
        for( const twGrooveResRecord &r : res )
            s.value.push_back( u < r.unitPower.size() ? r.unitPower[u] : 0.0f );
        out.push_back( std::move( s ) );
    }

    // --- rollnorm: compliance over a centered rolling-window peak ---------
    // Monotone-deque sliding maximum, O(nHops) total.
    {
        twGrooveMetricSeries s;
        s.id    = "rollnorm";
        s.label = "Resonance (rolling norm)";
        s.value.assign( nHops, 0.0f );

        const double hopSec    = (double)hopFrames / (double)rate;
        const long   halfHops  = std::max<long>( 1, (long)std::llround(
                                     0.5 * params.rollingNormSec / hopSec ) );
        std::vector<size_t> dq;   // indices, compliance decreasing
        size_t dqHead = 0;
        long   added  = -1;       // last index pushed into the deque
        for( size_t i = 0; i < nHops; i++ ) {
            const long lo = (long)i - halfHops;
            const long hi = std::min<long>( (long)nHops - 1, (long)i + halfHops );
            while( added < hi ) {
                added++;
                const float v = res[(size_t)added].compliance;
                while( dq.size() > dqHead && res[dq.back()].compliance <= v )
                    dq.pop_back();
                dq.push_back( (size_t)added );
            }
            while( dq.size() > dqHead && (long)dq[dqHead] < lo ) dqHead++;
            const double peak = dq.size() > dqHead ? (double)res[dq[dqHead]].compliance : 0.0;
            s.value[i] = peak > 0.0
                             ? (float)std::min( 1.0, (double)res[i].compliance / peak )
                             : 0.0f;
        }
        out.push_back( std::move( s ) );
    }

    // --- event-derived series ---------------------------------------------
    // In-category = |residual| <= fusionCeilingMs (section 2.3: past the
    // ceiling "compliance" is the wrong category -- that is articulation,
    // counted by `outliers`, never fed to sigma/mu).
    const auto inCategory = [&]( const twGrooveEvRecord &e ) {
        return std::fabs( (double)e.residualMs ) <= params.fusionCeilingMs;
    };

    // Global mu: median residual of every in-category event of the run.
    double globalMu = 0.0;
    {
        std::vector<double> all;
        all.reserve( ev.size() );
        for( const twGrooveEvRecord &e : ev )
            if( inCategory( e ) ) all.push_back( (double)e.residualMs );
        globalMu = medianOf( all );
    }

    twGrooveMetricSeries sigmaS, muDriftS, outlierS, evConfS, scoreS, densityS;
    sigmaS.id     = "sigma";    sigmaS.label    = "Timing jitter (sigma)";
    muDriftS.id   = "mudrift";  muDriftS.label  = "Feel stability (mu drift)";
    outlierS.id   = "outliers"; outlierS.label  = "Inside fusion ceiling";
    evConfS.id    = "evconf";   evConfS.label   = "Event confidence";
    scoreS.id     = "score";    scoreS.label    = "Score (compliance x sigma)";
    densityS.id   = "density";  densityS.label  = "Event density (the confound)";
    sigmaS.value.assign( nHops, kNoData );
    muDriftS.value.assign( nHops, kNoData );
    outlierS.value.assign( nHops, kNoData );
    evConfS.value.assign( nHops, kNoData );
    scoreS.value.assign( nHops, kNoData );
    densityS.value.assign( nHops, 0.0f );

    WindowCursor sigmaCur( ev ), muCur( ev );
    std::vector<double> scratch;
    std::vector<uint32_t> windowCounts( nHops, 0 );

    for( size_t i = 0; i < nHops; i++ ) {
        // -- the sigma window (also serves outliers / evconf / density) ----
        const HopWindow ws = windowAt( i, hopFrames, params.sigmaWindowSec, rate );
        sigmaCur.advanceTo( ws );
        uint32_t total = 0, past = 0;
        double confSum = 0.0;
        scratch.clear();
        for( size_t j = sigmaCur.begin; j < sigmaCur.end; j++ ) {
            total++;
            confSum += (double)ev[j].confidence;
            if( inCategory( ev[j] ) ) scratch.push_back( (double)ev[j].residualMs );
            else past++;
        }
        windowCounts[i] = total;

        if( total >= params.minWindowEvents ) {
            outlierS.value[i] = (float)( 1.0 - (double)past / (double)total );
            evConfS.value[i]  = (float)std::min( 1.0, std::max( 0.0, confSum / total ) );
        }
        double sigmaPenalty = 1.0;   // no data -> score falls back to compliance
        if( scratch.size() >= params.minWindowEvents ) {
            // Robust spread: 1.4826 * MAD around the window median. The
            // window median IS the local drift at this scale, so this is
            // drift-corrected by construction (section 11.5's rule).
            const double med = medianOf( scratch );
            for( double &v : scratch ) v = std::fabs( v - med );
            const double sigmaMs = 1.4826 * medianOf( scratch );
            const double mapped  = mapSigma( sigmaMs, params );
            sigmaS.value[i] = (float)mapped;
            sigmaPenalty    = mapped;
        }
        scoreS.value[i] = (float)std::min( 1.0, (double)res[i].compliance * sigmaPenalty );

        // -- the mu window (longer: phrase scale) --------------------------
        const HopWindow wm = windowAt( i, hopFrames, params.muWindowSec, rate );
        muCur.advanceTo( wm );
        scratch.clear();
        for( size_t j = muCur.begin; j < muCur.end; j++ )
            if( inCategory( ev[j] ) ) scratch.push_back( (double)ev[j].residualMs );
        if( scratch.size() >= params.minWindowEvents ) {
            const double localMu = medianOf( scratch );
            const double dev     = std::fabs( localMu - globalMu ) /
                                   std::max( 1e-9, params.feelBandMs );
            muDriftS.value[i] = (float)std::max( 0.0, 1.0 - dev );
        }
    }

    // density: windowed count over the run's max windowed count. Zero
    // events IS a real density (0.0), never the sentinel.
    uint32_t maxCount = 0;
    for( uint32_t c : windowCounts ) maxCount = std::max( maxCount, c );
    if( maxCount > 0 )
        for( size_t i = 0; i < nHops; i++ )
            densityS.value[i] = (float)windowCounts[i] / (float)maxCount;

    out.push_back( std::move( sigmaS ) );
    out.push_back( std::move( muDriftS ) );
    out.push_back( std::move( outlierS ) );
    out.push_back( std::move( evConfS ) );
    out.push_back( std::move( scoreS ) );
    out.push_back( std::move( densityS ) );

    // --- Tier B ("groove.dyn", proposal 40 M3c) ---------------------------
    // Appended ONLY when dyn records exist and cover the same hop grid as
    // res -- a pre-M3c store shows exactly the Tier A series above, never
    // sentinel-filled ghost rows. See the header for each series' law.
    if( !dyn.empty() && dyn.size() == nHops && !dyn[0].units.empty() ) {
        const uint32_t dynUnits = (uint32_t)dyn[0].units.size();
        const double   hopSec   = (double)hopFrames / (double)rate;

        // The reference unit: by name when the names are known, else the
        // first column (the default ensemble's own order).
        uint32_t refU = 0;
        for( uint32_t u = 0; u < dynUnits && u < unitNames.size(); u++ )
            if( unitNames[u] == "reference" ) { refU = u; break; }

        std::vector<double> supRaw( nHops ), tenRaw( nHops ),
                             slipRaw( nHops ), hypRaw( nHops );
        for( size_t i = 0; i < nHops; i++ ) {
            const twGrooveUnitDynSample &s =
                dyn[i].units[std::min<size_t>( refU, dyn[i].units.size() - 1 )];
            supRaw[i]  = s.support;
            tenRaw[i]  = s.tension;
            slipRaw[i] = s.slip;
            hypRaw[i]  = std::hypot( (double)s.support, (double)s.tension );
        }
        const PrefixMean supPre( supRaw ), tenPre( tenRaw ),
                          slipPre( slipRaw ), hypPre( hypRaw );

        const long halfSmooth = std::max<long>( 1, (long)std::llround(
                                    0.5 * params.dynSmoothSec / hopSec ) );
        const long halfLean   = std::max<long>( 1, (long)std::llround(
                                    0.5 * params.leanWindowSec / hopSec ) );

        // support / tension: smoothed, run-peak-|.|-normalized, 0.5-centered.
        auto pushSigned = [&]( const char *id, const char *label,
                               const PrefixMean &pre ) {
            std::vector<double> sm( nHops );
            double peak = 0.0;
            for( size_t i = 0; i < nHops; i++ ) {
                sm[i] = pre.mean( (long)i - halfSmooth, (long)i + halfSmooth );
                peak  = std::max( peak, std::fabs( sm[i] ) );
            }
            twGrooveMetricSeries s;
            s.id    = id;
            s.label = label;
            s.value.assign( nHops, 0.5f );
            if( peak > 0.0 )
                for( size_t i = 0; i < nHops; i++ )
                    s.value[i] = (float)( 0.5 + 0.5 * sm[i] / peak );
            out.push_back( std::move( s ) );
        };
        pushSigned( "support", "Drive support (in-phase)", supPre );
        pushSigned( "tension", "Counter-tension (quadrature)", tenPre );

        // lean: F-weighted mean of sin(phi) -- sum(tension)/sum(k*F) over
        // the lean window; the confound-free lean, sentinel where the
        // window carries essentially no drive at all.
        {
            twGrooveMetricSeries s;
            s.id    = "lean";
            s.label = "Lean (F-weighted sin dphi)";
            s.value.assign( nHops, kNoData );
            for( size_t i = 0; i < nHops; i++ ) {
                const double sumH = hypPre.sum( (long)i - halfLean,
                                                (long)i + halfLean );
                if( sumH <= 1e-9 ) continue;
                const double lean = tenPre.sum( (long)i - halfLean,
                                                (long)i + halfLean ) / sumH;
                s.value[i] = (float)( 0.5 + 0.5 * std::max( -1.0,
                                                 std::min( 1.0, lean ) ) );
            }
            out.push_back( std::move( s ) );
        }

        // slip: windowed mean, 1 - clamp(mean/slipCap).
        {
            twGrooveMetricSeries s;
            s.id    = "slip";
            s.label = "Phase lock (1 - slip)";
            s.value.assign( nHops, 0.0f );
            for( size_t i = 0; i < nHops; i++ ) {
                const double m = slipPre.mean( (long)i - halfSmooth,
                                               (long)i + halfSmooth );
                const double c = params.slipCap > 0.0
                                   ? std::min( 1.0, std::max( 0.0, m / params.slipCap ) )
                                   : 1.0;
                s.value[i] = (float)( 1.0 - c );
            }
            out.push_back( std::move( s ) );
        }

        // move:<unit>: dissipated power over the unit's own run peak --
        // the section 3.4 per-body-part display.
        for( uint32_t u = 0; u < dynUnits; u++ ) {
            const std::string name =
                u < unitNames.size() && !unitNames[u].empty()
                    ? unitNames[u] : "unit" + std::to_string( u );
            twGrooveMetricSeries s;
            s.id    = "move:" + name;
            s.label = "Movement " + name;
            s.value.assign( nHops, 0.0f );
            double peak = 0.0;
            for( size_t i = 0; i < nHops; i++ )
                if( u < dyn[i].units.size() )
                    peak = std::max( peak, (double)dyn[i].units[u].dissip );
            if( peak > 0.0 )
                for( size_t i = 0; i < nHops; i++ )
                    if( u < dyn[i].units.size() )
                        s.value[i] = (float)std::min(
                            1.0, (double)dyn[i].units[u].dissip / peak );
            out.push_back( std::move( s ) );
        }
    }
    return out;
}
