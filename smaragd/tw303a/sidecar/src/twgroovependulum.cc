#include "tw/sidecar/twgroovependulum.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <utility>

// The pendulum ensemble (proposal 40 M0). Pure, deterministic, single-
// threaded: no threads, no rand(), fixed iteration order. NORMATIVE per
// twgroovependulum.h -- see that file for the per-hop update equations.

namespace {

constexpr double kPi = 3.14159265358979323846;

double wrapSigned( double x, double half )
{
    double y = std::fmod( x + half, 2.0 * half );
    if( y < 0.0 ) y += 2.0 * half;
    return y - half;
}

std::vector<double> makeReceptiveField( twGrooveReceptiveShape shape, uint32_t nRegions )
{
    std::vector<double> w( nRegions, 0.0 );
    if( nRegions == 0 ) return w;
    double sum = 0.0;
    for( uint32_t r = 0; r < nRegions; r++ ) {
        switch( shape ) {
            case twGrooveReceptiveShape::LowHeavy:  w[r] = 1.0 / (double)( r + 1 ); break;
            case twGrooveReceptiveShape::HighHeavy: w[r] = (double)( r + 1 );        break;
            case twGrooveReceptiveShape::Broad:
            default:                                w[r] = 1.0;                     break;
        }
        sum += w[r];
    }
    if( sum > 0.0 )
        for( uint32_t r = 0; r < nRegions; r++ ) w[r] /= sum;
    return w;
}

// Cumulative (unwrapped) phase from a per-hop wrapped-phase trajectory --
// standard technique: accumulate the wrapped delta between consecutive hops.
std::vector<double> unwrapPhase( const std::vector<double> &wrapped )
{
    std::vector<double> u( wrapped.size(), 0.0 );
    if( wrapped.empty() ) return u;
    u[0] = wrapped[0];
    for( size_t i = 1; i < wrapped.size(); i++ )
        u[i] = u[i - 1] + wrapSigned( wrapped[i] - wrapped[i - 1], kPi );
    return u;
}

// Linear interpolation of an array at a fractional hop index, clamped to the
// array's own domain.
double lerpAt( const std::vector<double> &arr, double hopIdxF )
{
    if( arr.empty() ) return 0.0;
    if( hopIdxF <= 0.0 ) return arr.front();
    const double lastIdx = (double)arr.size() - 1.0;
    if( hopIdxF >= lastIdx ) return arr.back();
    const size_t i0 = (size_t)hopIdxF;
    const size_t i1 = std::min( i0 + 1, arr.size() - 1 );
    const double frac = hopIdxF - (double)i0;
    return arr[i0] * ( 1.0 - frac ) + arr[i1] * frac;
}

// Runs pass 1 (adaptive dynamics for every unit) over `field`, seeded from
// `tatumPeriodSec`. Returns one trajectory per params.ensemble entry, in
// ensemble order.
std::vector<twGroovePendulumUnitTrajectory> runPass1( const twGrooveField &field,
                                                       double tatumPeriodSec,
                                                       const twGroovePendulumParams &params )
{
    const uint32_t nHops    = field.nHops;
    const uint32_t nRegions = (uint32_t)field.regionLowHz.size();
    const double   dt       = (double)field.hopFrames / (double)field.rate;

    // Bar period seed: the ABSOLUTE anchor (params.defaultBarPeriodSec's
    // doc) -- never a tatum multiple, which cannot distinguish an
    // eighth-note tatum's 8-tatum bar from a quarter-note tatum's 4-tatum
    // bar without knowing which the material has.
    const double barPeriodSec = params.defaultBarPeriodSec;

    std::vector<twGroovePendulumUnitTrajectory> out;
    out.reserve( params.ensemble.size() );

    for( const twGroovePendulumUnitSpec &spec : params.ensemble ) {
        twGroovePendulumUnitTrajectory traj;
        traj.name = spec.name;
        traj.phaseWrapped.assign( nHops, 0.0 );
        traj.omega.assign( nHops, 0.0 );
        traj.magnitude.assign( nHops, 0.0 );
        traj.driveF.assign( nHops, 0.0 );

        const double omega0 =
            ( spec.periodInBars > 0.0 && barPeriodSec > 0.0 )
                ? 2.0 * kPi / ( spec.periodInBars * barPeriodSec )
                : ( spec.periodInTatums > 0.0 && tatumPeriodSec > 0.0 )
                      ? 2.0 * kPi / ( spec.periodInTatums * tatumPeriodSec )
                      : 0.0;
        const double alpha     = ( spec.dampingCycles > 0.0 && omega0 > 0.0 )
                                   ? -omega0 / ( 2.0 * kPi * spec.dampingCycles )
                                   : -1.0;
        const double omegaMin  = omega0 / std::sqrt( 2.0 );
        const double omegaMax  = omega0 * std::sqrt( 2.0 );
        traj.omega0             = omega0;

        const std::vector<double> w = makeReceptiveField( spec.receptive, nRegions );

        // F_p(t), precomputed for the whole run. For a BAR-SEEDED unit only
        // (periodInBars > 0: sway, twobar), its MEAN is removed before it
        // drives the dynamics below -- flux is HALF-WAVE RECTIFIED
        // (twgroove.h step 6: never negative), so it always carries a
        // positive DC component whose SIZE tracks how much content a region
        // has, independent of whether that content is periodic at this
        // unit's own rate. Measured on AC (d): a linear damped oscillator
        // driven by a signal with a large DC term develops a large steady
        // offset from that DC alone (the same physics as a pendulum pushed
        // by a constant force, not a periodic one), and a dense broadband
        // fixture's (a0) DC level swamped a slow unit's response so
        // completely that fixture (d)'s genuinely 2-bar-periodic drive READ
        // AS LESS resonant than fixture (a0)'s uniform one -- backwards from
        // what AC (d) requires.
        //
        // Scoped to the bar-seeded units ONLY, not applied globally: tried
        // it on every unit first and it broke the REFERENCE unit's phase
        // tracking under a moving tempo -- AC (c)'s tempo ramp and AC (i)'s
        // tempo-change boundary both got measurably WORSE (AC (i)'s phantom
        // shift went from a passing 1.9 ms to 10.8 ms). The reference is
        // fast enough (tatum-rate) that its own AC component already
        // dominates a0-style dense drive, so it never had the DC problem;
        // subtracting a GLOBAL mean from it under a drifting tempo instead
        // introduced a bias the adaptive omega law had to fight throughout
        // the run. The RAW F_p(t) is always what traj.driveF and section
        // 3.5's <F> report, for every unit -- the mean removal, when it
        // applies at all, is local to the DYNAMICS input only.
        std::vector<double> rawF( nHops, 0.0 );
        for( uint32_t hop = 0; hop < nHops; hop++ ) {
            double F = 0.0;
            for( uint32_t r = 0; r < nRegions; r++ )
                F += w[r] * field.regionFlux[(size_t)r * (size_t)nHops + hop];
            rawF[hop] = F;
        }
        double meanF = 0.0;
        if( spec.periodInBars > 0.0 ) {
            for( double v : rawF ) meanF += v;
            meanF = nHops ? meanF / (double)nHops : 0.0;
        }

        std::complex<double> z( 0.0, 0.0 );
        double omega = omega0;
        for( uint32_t hop = 0; hop < nHops; hop++ ) {
            const double F   = rawF[hop];
            const double Fac = F - meanF;

            // EXACT per-step update for the LINEAR part (z's own
            // alpha+i*omega term), not forward Euler. Found necessary: at
            // the reference unit's own tatum-rate omega (~25 rad/s at a
            // 250 ms tatum) and dt = 1/envRateHz = 5 ms, forward Euler's
            // own per-step growth factor |1 + (alpha+i*omega)*dt| measured
            // 1.0029 -- ABOVE 1 even though alpha < 0 (damping), because dt*
            // omega (~0.126 rad) is not small enough for the linear
            // approximation "1 + lambda*dt" to stay inside forward Euler's
            // stability region for an oscillatory system. Compounded over a
            // multi-second run that is not a rounding error: measured
            // 1.0029^7600 (a 38 s fixture at this hop rate) = ~3.7e9, i.e.
            // the INTEGRATOR ITSELF was injecting unbounded spurious growth
            // into every unit's |z|, independent of any real drive -- found
            // via AC (h): the reference's confidence never recovered after
            // the break because it was still numerically "charging" at the
            // run's end regardless of when coherent drive actually resumed.
            // z(t+dt) = z(t)*exp(lambda*dt) + k*Fac*(exp(lambda*dt)-1)/lambda
            // is the closed-form solution for constant Fac over one hop and
            // carries NO discretization error in the linear term at any dt.
            const std::complex<double> lambda( alpha, omega );
            const std::complex<double> ea = std::exp( lambda * dt );
            const std::complex<double> stepGain = ( std::abs( lambda ) > 1e-9 ) ? ( ea - 1.0 ) / lambda
                                                                                  : std::complex<double>( dt, 0.0 );
            z = z * ea + ( spec.k * Fac ) * stepGain;
            omega += -dt * spec.eps * spec.k * Fac * std::sin( std::arg( z ) );
            if( omega0 > 0.0 ) {
                if( omega < omegaMin ) omega = omegaMin;
                if( omega > omegaMax ) omega = omegaMax;
            }

            traj.phaseWrapped[hop] = std::arg( z );
            traj.omega[hop]        = omega;
            traj.magnitude[hop]    = std::abs( z );
            traj.driveF[hop]       = F;
        }

        out.push_back( std::move( traj ) );
    }
    return out;
}

int findUnitIndex( const std::vector<twGroovePendulumUnitTrajectory> &trajs, const char *name )
{
    for( size_t i = 0; i < trajs.size(); i++ )
        if( trajs[i].name == name ) return (int)i;
    return trajs.empty() ? -1 : 0;
}

twGrooveBaselineParams tatumSeedParams( const twGroovePendulumParams &params )
{
    twGrooveBaselineParams bp;
    bp.minTatumSec = params.minTatumSec;
    bp.maxTatumSec = params.maxTatumSec;
    return bp;
}

} // namespace

std::vector<twGroovePendulumUnitSpec> twGrooveDefaultEnsemble()
{
    std::vector<twGroovePendulumUnitSpec> e;
    twGroovePendulumUnitSpec u;

    // "reference": tatum-rate, broad-field -- section 3.3's common reference
    // (the gauge every residual is measured against). Broad and tatum-rate
    // so it is not itself biased toward the low or the high band.
    u.name = "reference"; u.periodInTatums = 1.0;  u.receptive = twGrooveReceptiveShape::Broad;
    u.k = 1.5; u.eps = 0.05; u.dampingCycles = 4.0;
    e.push_back( u );

    // "bounce": beat-rate, low-heavy (Hove 2014 -- low-band timing/tapping
    // dominance; Toiviainen 2010 -- vertical bounce at the beat).
    u.name = "bounce"; u.periodInTatums = 2.0; u.receptive = twGrooveReceptiveShape::LowHeavy;
    u.k = 1.5; u.eps = 0.05; u.dampingCycles = 4.0;
    e.push_back( u );

    // "limbs": half-bar, high-percussive (Burger 2013 -- high-band flux /
    // percussiveness drives hand movement).
    u.name = "limbs"; u.periodInTatums = 4.0; u.receptive = twGrooveReceptiveShape::HighHeavy;
    u.k = 1.5; u.eps = 0.05; u.dampingCycles = 4.0;
    e.push_back( u );

    // "sway": bar-rate, broad (Toiviainen 2010 -- torso sway at the bar).
    // periodInBars (not periodInTatums): a bar is not reliably a fixed
    // tatum-multiple -- see twGroovePendulumUnitSpec::periodInBars's doc. A
    // longer damping memory than bounce/limbs -- it is meant to integrate
    // over several bars, not track a single one.
    u.name = "sway"; u.periodInTatums = 0.0; u.periodInBars = 1.0;
    u.receptive = twGrooveReceptiveShape::Broad;
    u.k = 1.5; u.eps = 0.04; u.dampingCycles = 6.0;
    e.push_back( u );

    // "twobar": the multi-bar unit AC (d) exercises directly. Bar-seeded, as
    // "sway" above. Higher k and longer damping memory than the others --
    // tuned (per the M0 house rule, "tune ONLY the 2-bar unit's omega
    // register/coupling") because a unit this slow needs to accumulate
    // several cycles of a genuinely 2-bar-periodic drive before it separates
    // from noise, and starting it at the SAME k/dampingCycles as the fast
    // units measured indistinguishable from fixture (a0)'s broadband-every-
    // eighth drive.
    u.name = "twobar"; u.periodInTatums = 0.0; u.periodInBars = 2.0;
    u.receptive = twGrooveReceptiveShape::Broad;
    u.k = 3.5; u.eps = 0.02; u.dampingCycles = 10.0;
    e.push_back( u );

    return e;
}

twGroovePendulumResult twGroovePendulumAnalyze( const twGrooveField &field,
                                                const twGroovePendulumParams &params )
{
    twGroovePendulumResult result;
    result.hopFrames = field.hopFrames;
    result.rate      = field.rate;
    if( field.events.empty() || field.nHops == 0 || field.rate == 0 || params.ensemble.empty() )
        return result;

    const twGrooveBaselineResult seed = twGrooveBaselineAnalyze( field, tatumSeedParams( params ) );
    if( seed.tatumPeriodSec <= 0.0 )
        return result;
    result.tatumPeriodSec = seed.tatumPeriodSec;

    // --- Pass 1: adaptive dynamics for every unit, over the whole material. ---
    std::vector<twGroovePendulumUnitTrajectory> trajs = runPass1( field, seed.tatumPeriodSec, params );
    const int refIdx = findUnitIndex( trajs, "reference" );
    if( refIdx < 0 ) return result;

    const std::vector<double> refUnwrapped = unwrapPhase( trajs[(size_t)refIdx].phaseWrapped );

    // --- Confidence: the reference unit's own driven resonance, normalized
    // by the run's own peak (design section 3.3/AC (h): dips during a fill
    // or a break, where coherent periodic drive is absent). ---
    double peakMagSq = 0.0;
    for( double m : trajs[(size_t)refIdx].magnitude ) peakMagSq = std::max( peakMagSq, m * m );
    result.confidence.assign( field.nHops, 0.0 );
    if( peakMagSq > 0.0 )
        for( uint32_t hop = 0; hop < field.nHops; hop++ ) {
            const double m = trajs[(size_t)refIdx].magnitude[hop];
            result.confidence[hop] = std::min( 1.0, ( m * m ) / peakMagSq );
        }

    // --- Pass 2: score every event against the FROZEN reference trajectory. ---
    const uint32_t nRegions = (uint32_t)field.regionLowHz.size();
    std::vector<std::vector<twGrooveScoredEvent>> eventsByRegion( nRegions );
    const double rate = (double)field.rate;

    for( const twGrooveEvent &ev : field.events ) {
        if( ev.region >= nRegions ) continue;
        const double hopIdxF = ev.posFrames / (double)field.hopFrames;
        const double phaseAtEvent = lerpAt( refUnwrapped, hopIdxF );
        const double omegaAtEvent = lerpAt( trajs[(size_t)refIdx].omega, hopIdxF );
        if( omegaAtEvent <= 0.0 ) continue;
        const double nearestTick   = std::round( phaseAtEvent / ( 2.0 * kPi ) ) * 2.0 * kPi;
        const double residPhase    = phaseAtEvent - nearestTick;
        const double localPeriodSec = 2.0 * kPi / omegaAtEvent;
        const double residMs        = residPhase / ( 2.0 * kPi ) * localPeriodSec * 1000.0;
        eventsByRegion[ev.region].push_back( { ev.posFrames / rate, residMs, ev.amp } );
    }

    const double totalSec = (double)field.nHops * (double)field.hopFrames / rate;
    result.residuals = twGroovePoolRegionStats( eventsByRegion, totalSec, params.stats );

    // --- Per-unit summaries: mean R = mean(|z|^2), and the section 3.5
    // counter-tension readout (pass-2-only BY CONSTRUCTION: it is read off
    // the FROZEN pass-1 trajectory, never re-integrated). ---
    result.unitTrajectories = trajs;
    result.unitMeanR.assign( trajs.size(), 0.0 );
    result.counterTension.assign( trajs.size(), twGrooveCounterTension{} );
    for( size_t u = 0; u < trajs.size(); u++ ) {
        double sumRsq = 0.0, sumSin = 0.0, sumF = 0.0;
        const size_t n = trajs[u].magnitude.size();
        for( size_t hop = 0; hop < n; hop++ ) {
            sumRsq += trajs[u].magnitude[hop] * trajs[u].magnitude[hop];
            sumSin += std::sin( trajs[u].phaseWrapped[hop] );
            sumF   += trajs[u].driveF[hop];
        }
        const double meanRsq = n ? sumRsq / (double)n : 0.0;
        const double meanSin = n ? sumSin / (double)n : 0.0;
        const double meanF   = n ? sumF / (double)n : 0.0;
        double varSin = 0.0;
        for( size_t hop = 0; hop < n; hop++ ) {
            const double d = std::sin( trajs[u].phaseWrapped[hop] ) - meanSin;
            varSin += d * d;
        }
        varSin = n ? varSin / (double)n : 0.0;

        result.unitMeanR[u] = meanRsq;
        result.counterTension[u].name            = trajs[u].name;
        result.counterTension[u].meanSinDeltaPhi  = meanSin;
        result.counterTension[u].varSinDeltaPhi    = varSin;
        result.counterTension[u].meanF             = meanF;
    }

    return result;
}

twGrooveTrainedStructure twGroovePendulumTrainStructure( const twGrooveField &trainField,
                                                          const twGroovePendulumParams &params )
{
    twGrooveTrainedStructure structure;
    structure.paramsUsed = params;

    const twGroovePendulumResult trained = twGroovePendulumAnalyze( trainField, params );
    const uint32_t nRegions = (uint32_t)trainField.regionLowHz.size();
    structure.trainedHasRegion.assign( nRegions, false );
    structure.trainedMuMsByRegion.assign( nRegions, 0.0 );
    for( uint32_t r = 0; r < nRegions && r < trained.residuals.perRegion.size(); r++ ) {
        structure.trainedHasRegion[r]     = trained.residuals.perRegion[r].hasData;
        structure.trainedMuMsByRegion[r]  = trained.residuals.perRegion[r].muMs;
    }
    return structure;
}

twGroovePendulumResult twGroovePendulumScoreWithStructure( const twGrooveField &scoreField,
                                                            const twGrooveTrainedStructure &structure )
{
    // The frozen structure is the ensemble SPEC (receptive fields, period
    // RATIOS) carried in structure.paramsUsed; omega is re-seeded from
    // scoreField's OWN baseline tatum inside twGroovePendulumAnalyze exactly
    // as free-running mode does -- design section 3.2: freezing the clock,
    // not just the structure, would turn a tempo difference into a phantom
    // lean. The trained mu(region) pattern is left on `structure` for the
    // CALLER to diff against the returned residuals (see twgroovependulum.h).
    return twGroovePendulumAnalyze( scoreField, structure.paramsUsed );
}
