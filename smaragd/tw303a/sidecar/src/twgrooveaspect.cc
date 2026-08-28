#include "tw/sidecar/twgrooveaspect.h"

#include <algorithm>
#include <cmath>
#include <cstring>

// Proposal 40 "Feel Flow" M1 -- see twgrooveaspect.h for the module doc.
// This file NEVER touches twGrooveAnalyzeFrontEnd / twGroovePendulumAnalyze's
// internals; it only calls them and reshapes the result.

namespace {

// -----------------------------------------------------------------------
// LE byte helpers -- house convention (tw/sidecar/twanalyzers.cc's
// putU32/putF32), extended here with the u16/u64/f64 variants this blob
// needs. memcpy for the float/double bit reinterpretation (never a cast --
// that would be UB on the aliasing rule).
// -----------------------------------------------------------------------
void putU16( std::vector<uint8_t> &out, uint16_t v )
{
    out.push_back( (uint8_t)( v & 0xff ) );
    out.push_back( (uint8_t)( ( v >> 8 ) & 0xff ) );
}

void putU32( std::vector<uint8_t> &out, uint32_t v )
{
    for( int i = 0; i < 4; i++ )
        out.push_back( (uint8_t)( ( v >> ( 8 * i ) ) & 0xff ) );
}

void putU64( std::vector<uint8_t> &out, uint64_t v )
{
    for( int i = 0; i < 8; i++ )
        out.push_back( (uint8_t)( ( v >> ( 8 * i ) ) & 0xff ) );
}

void putF32( std::vector<uint8_t> &out, float v )
{
    uint32_t bits;
    memcpy( &bits, &v, 4 );
    putU32( out, bits );
}

void putF64( std::vector<uint8_t> &out, double v )
{
    uint64_t bits;
    memcpy( &bits, &v, 8 );
    putU64( out, bits );
}

uint16_t getU16( const uint8_t *p )
{
    return (uint16_t)( (uint16_t)p[0] | ( (uint16_t)p[1] << 8 ) );
}

uint32_t getU32( const uint8_t *p )
{
    uint32_t v = 0;
    for( int i = 3; i >= 0; i-- ) v = ( v << 8 ) | p[i];
    return v;
}

uint64_t getU64( const uint8_t *p )
{
    uint64_t v = 0;
    for( int i = 7; i >= 0; i-- ) v = ( v << 8 ) | p[i];
    return v;
}

float getF32( const uint8_t *p )
{
    uint32_t bits = getU32( p );
    float    v;
    memcpy( &v, &bits, 4 );
    return v;
}

double getF64( const uint8_t *p )
{
    uint64_t bits = getU64( p );
    double   v;
    memcpy( &v, &bits, 8 );
    return v;
}

// Linear interpolation of an array at a fractional index, clamped to the
// array's own domain -- a generic reduction (identical in spirit to, but a
// separate implementation from, twgroovependulum.cc's private lerpAt: that
// one is part of pass 2's scoring math, this one is aspect-encoder plumbing
// resampling an ALREADY-COMPUTED trajectory onto a different hop grid).
double lerpArr( const std::vector<double> &arr, double idxF )
{
    if( arr.empty() ) return 0.0;
    if( idxF <= 0.0 ) return arr.front();
    const double lastIdx = (double)arr.size() - 1.0;
    if( idxF >= lastIdx ) return arr.back();
    const size_t i0   = (size_t)idxF;
    const size_t i1   = std::min( i0 + 1, arr.size() - 1 );
    const double frac = idxF - (double)i0;
    return arr[i0] * ( 1.0 - frac ) + arr[i1] * frac;
}

} // namespace

// ---------------------------------------------------------------------------
// twGrooveAnalysisParams::serialize -- canonical LE params blob. Field
// order (normative, twaspects.h):
//   front end:   u32 nBands, f32 fMinHz, f32 fMaxHz, f32 envRateHz,
//                u32 nRegions, f32 medianHalfWidthSec, f32 thresholdFactor,
//                f32 energyFloorFraction, f32 minSeparationSec
//   ensemble:    u32 count, then per unit (in order):
//                  u32 nameLen, nameLen raw bytes (no NUL),
//                  f64 periodInTatums, u8 receptive (0 Broad/1 LowHeavy/
//                  2 HighHeavy), f64 periodInBars, f64 k, f64 eps,
//                  f64 dampingCycles
//   pendulum:    f64 minTatumSec, f64 maxTatumSec, f64 defaultBarPeriodSec,
//                f64 confidenceFloor
//   stats:       f64 driftWindowSec, f64 driftStepSec, f64 bimodalMinGapMs,
//                f64 bimodalMinFrac, f64 bleedGateDb
// ---------------------------------------------------------------------------
void twGrooveAnalysisParams::serialize( std::vector<uint8_t> &out ) const
{
    out.clear();

    const twGrooveFrontEndParams &fe = frontEnd;
    putU32( out, fe.nBands );
    putF32( out, fe.fMinHz );
    putF32( out, fe.fMaxHz );
    putF32( out, fe.envRateHz );
    putU32( out, fe.nRegions );
    putF32( out, fe.medianHalfWidthSec );
    putF32( out, fe.thresholdFactor );
    putF32( out, fe.energyFloorFraction );
    putF32( out, fe.minSeparationSec );

    putU32( out, (uint32_t)pendulum.ensemble.size() );
    for( const twGroovePendulumUnitSpec &u : pendulum.ensemble ) {
        putU32( out, (uint32_t)u.name.size() );
        out.insert( out.end(), u.name.begin(), u.name.end() );
        putF64( out, u.periodInTatums );
        out.push_back( (uint8_t)u.receptive );
        putF64( out, u.periodInBars );
        putF64( out, u.k );
        putF64( out, u.eps );
        putF64( out, u.dampingCycles );
    }

    putF64( out, pendulum.minTatumSec );
    putF64( out, pendulum.maxTatumSec );
    putF64( out, pendulum.defaultBarPeriodSec );
    putF64( out, pendulum.confidenceFloor );

    const twGrooveStatsParams &st = pendulum.stats;
    putF64( out, st.driftWindowSec );
    putF64( out, st.driftStepSec );
    putF64( out, st.bimodalMinGapMs );
    putF64( out, st.bimodalMinFrac );
    putF64( out, st.bleedGateDb );

    // Proposal 40 M3: appended ADDITIVELY and ONLY for mode == Trained (see
    // the struct's own doc) -- a default-constructed params blob (every
    // M1/M1b/M2 caller, and every track that has never touched Feel Flow's
    // mode) stops exactly here, byte-identical to the pre-M3 sequence above.
    if( mode == twGrooveMode::Trained ) {
        out.push_back( 1 );   // trained-mode marker; NEVER written for Adaptive
        std::vector<uint8_t> tsBlob;
        twGrooveTrainedStructureSerialize( trained, tsBlob );
        putU32( out, (uint32_t) tsBlob.size() );
        out.insert( out.end(), tsBlob.begin(), tsBlob.end() );
    }
}

// ---------------------------------------------------------------------------
// twGrooveTrainedStructureSerialize / Deserialize -- see twgrooveaspect.h's
// doc for the normative field order. A small local helper for the ensemble
// block only, deliberately NOT shared with twGrooveAnalysisParams::serialize
// above: that function's byte-exactness is the whole M1/M1b/M2 gate, and a
// shared helper would mean a change made for THIS struct's sake could move
// bytes there too. Two independent (if textually similar) encoders is the
// safer trade here.
// ---------------------------------------------------------------------------
namespace {
void putTrainedEnsemble( std::vector<uint8_t> &out,
                         const std::vector<twGroovePendulumUnitSpec> &ensemble )
{
    putU32( out, (uint32_t) ensemble.size() );
    for( const twGroovePendulumUnitSpec &u : ensemble ) {
        putU32( out, (uint32_t) u.name.size() );
        out.insert( out.end(), u.name.begin(), u.name.end() );
        putF64( out, u.periodInTatums );
        out.push_back( (uint8_t) u.receptive );
        putF64( out, u.periodInBars );
        putF64( out, u.k );
        putF64( out, u.eps );
        putF64( out, u.dampingCycles );
    }
}
} // namespace

void twGrooveTrainedStructureSerialize( const twGrooveTrainedStructure &s,
                                        std::vector<uint8_t> &out )
{
    out.clear();
    putTrainedEnsemble( out, s.paramsUsed.ensemble );
    putF64( out, s.paramsUsed.minTatumSec );
    putF64( out, s.paramsUsed.maxTatumSec );
    putF64( out, s.paramsUsed.defaultBarPeriodSec );
    putF64( out, s.paramsUsed.confidenceFloor );
    const twGrooveStatsParams &st = s.paramsUsed.stats;
    putF64( out, st.driftWindowSec );
    putF64( out, st.driftStepSec );
    putF64( out, st.bimodalMinGapMs );
    putF64( out, st.bimodalMinFrac );
    putF64( out, st.bleedGateDb );

    putU32( out, (uint32_t) s.trainedHasRegion.size() );
    for( bool b : s.trainedHasRegion ) out.push_back( b ? 1 : 0 );
    putU32( out, (uint32_t) s.trainedMuMsByRegion.size() );
    for( double v : s.trainedMuMsByRegion ) putF64( out, v );
}

bool twGrooveTrainedStructureDeserialize( const uint8_t *data, uint64_t len,
                                          twGrooveTrainedStructure &out )
{
    out = twGrooveTrainedStructure();
    if( data == nullptr ) return false;

    uint64_t pos = 0;
    auto need = [&]( uint64_t n ) { return pos + n <= len; };

    if( !need( 4 ) ) return false;
    const uint32_t nUnits = getU32( data + pos ); pos += 4;
    out.paramsUsed.ensemble.resize( nUnits );
    for( uint32_t i = 0; i < nUnits; i++ ) {
        if( !need( 4 ) ) return false;
        const uint32_t nameLen = getU32( data + pos ); pos += 4;
        if( !need( nameLen ) ) return false;
        twGroovePendulumUnitSpec &u = out.paramsUsed.ensemble[i];
        u.name.assign( (const char *) ( data + pos ), nameLen ); pos += nameLen;
        if( !need( 8 ) ) return false;
        u.periodInTatums = getF64( data + pos ); pos += 8;
        if( !need( 1 ) ) return false;
        u.receptive = (twGrooveReceptiveShape) data[pos]; pos += 1;
        if( !need( 32 ) ) return false;
        u.periodInBars   = getF64( data + pos ); pos += 8;
        u.k              = getF64( data + pos ); pos += 8;
        u.eps            = getF64( data + pos ); pos += 8;
        u.dampingCycles  = getF64( data + pos ); pos += 8;
    }

    if( !need( 32 ) ) return false;
    out.paramsUsed.minTatumSec        = getF64( data + pos ); pos += 8;
    out.paramsUsed.maxTatumSec        = getF64( data + pos ); pos += 8;
    out.paramsUsed.defaultBarPeriodSec = getF64( data + pos ); pos += 8;
    out.paramsUsed.confidenceFloor    = getF64( data + pos ); pos += 8;

    if( !need( 40 ) ) return false;
    twGrooveStatsParams &st = out.paramsUsed.stats;
    st.driftWindowSec  = getF64( data + pos ); pos += 8;
    st.driftStepSec    = getF64( data + pos ); pos += 8;
    st.bimodalMinGapMs = getF64( data + pos ); pos += 8;
    st.bimodalMinFrac  = getF64( data + pos ); pos += 8;
    st.bleedGateDb     = getF64( data + pos ); pos += 8;

    if( !need( 4 ) ) return false;
    const uint32_t nHasRegion = getU32( data + pos ); pos += 4;
    if( !need( nHasRegion ) ) return false;
    out.trainedHasRegion.resize( nHasRegion );
    for( uint32_t i = 0; i < nHasRegion; i++ ) out.trainedHasRegion[i] = data[pos++] != 0;

    if( !need( 4 ) ) return false;
    const uint32_t nMu = getU32( data + pos ); pos += 4;
    if( !need( (uint64_t) nMu * 8 ) ) return false;
    out.trainedMuMsByRegion.resize( nMu );
    for( uint32_t i = 0; i < nMu; i++ ) {
        out.trainedMuMsByRegion[i] = getF64( data + pos ); pos += 8;
    }

    return true;
}

// ---------------------------------------------------------------------------
// twGrooveBuildAspectPayloads
// ---------------------------------------------------------------------------
twGrooveAspectPayloads twGrooveBuildAspectPayloads(
    const float *const *chans, uint32_t nCh, uint64_t nFrames, uint32_t rate,
    const twGrooveAnalysisParams &params )
{
    twGrooveAspectPayloads out;
    if( chans == nullptr || nCh == 0 || nFrames == 0 || rate == 0 )
        return out;

    const twGrooveField field =
        twGrooveAnalyzeFrontEnd( chans, nCh, nFrames, rate, params.frontEnd );
    if( field.nHops == 0 )
        return out;   // invalid front-end config -- see twGrooveAnalyzeFrontEnd's doc

    // Proposal 40 M3: Trained mode scores against a frozen structure
    // (design section 3.2 -- training freezes the STRUCTURE, never the
    // clock: omega is always re-seeded from THIS field, never frozen).
    // trainedHasRegion.empty() -- NOT paramsUsed.ensemble.empty(), which
    // twGroovePendulumParams's own default member initializer (the 5-unit
    // twGrooveDefaultEnsemble()) makes true even for a NEVER-trained
    // structure -- means "requested Trained but twGroovePendulumTrainStructure
    // has never run for this track": fall back to the ordinary Adaptive path
    // rather than mis-score against a structure that was never fit.
    const bool useTrained = params.mode == twGrooveMode::Trained
                           && !params.trained.trainedHasRegion.empty();
    const twGroovePendulumResult result = useTrained
        ? twGroovePendulumScoreWithStructure( field, params.trained )
        : twGroovePendulumAnalyze( field, params.pendulum );
    // Honest-empty: no recoverable tatum, or no "reference" unit in the
    // ensemble -- twGroovePendulumAnalyze's own bail conditions.
    if( result.tatumPeriodSec <= 0.0 || result.unitTrajectories.empty() )
        return out;

    const uint32_t nUnits = (uint32_t)result.unitTrajectories.size();
    out.nUnits    = nUnits;
    out.hopFrames = std::max<uint32_t>( 1, rate / 100 );   // AC 1: fixed 10 ms hop

    // Per-unit peak power over the whole run -- the normalization divisor
    // (twaspects.h's "groove.res" doc). 0 stays 0 (a unit that never moved).
    std::vector<double> peakMagSq( nUnits, 0.0 );
    for( uint32_t u = 0; u < nUnits; u++ )
        for( double m : result.unitTrajectories[u].magnitude )
            peakMagSq[u] = std::max( peakMagSq[u], m * m );

    const uint64_t nRecs =
        ( nFrames + out.hopFrames - 1 ) / out.hopFrames;   // ceil, "loudness"'s convention
    out.resRecordCount = nRecs;
    out.resPayload.reserve( (size_t)nRecs * ( nUnits + 1 ) * 4 );

    const double pendulumHopFrames =
        result.hopFrames > 0 ? (double)result.hopFrames : (double)field.hopFrames;

    for( uint64_t k = 0; k < nRecs; k++ ) {
        const double framePos = (double)( k * (uint64_t)out.hopFrames );
        const double hopIdxF  = pendulumHopFrames > 0.0 ? framePos / pendulumHopFrames : 0.0;
        for( uint32_t u = 0; u < nUnits; u++ ) {
            const double mag = lerpArr( result.unitTrajectories[u].magnitude, hopIdxF );
            const double p   = peakMagSq[u] > 0.0
                                  ? std::min( 1.0, ( mag * mag ) / peakMagSq[u] )
                                  : 0.0;
            putF32( out.resPayload, (float)p );
        }
        const double compl_ =
            std::min( 1.0, std::max( 0.0, lerpArr( result.confidence, hopIdxF ) ) );
        putF32( out.resPayload, (float)compl_ );
    }

    out.evRecordCount = (uint64_t)result.scoredEvents.size();
    out.evPayload.reserve( (size_t)out.evRecordCount * 20 );
    for( const twGrooveScoredEventRecord &se : result.scoredEvents ) {
        const double hopIdxF =
            pendulumHopFrames > 0.0 ? se.posFrames / pendulumHopFrames : 0.0;
        const double conf =
            std::min( 1.0, std::max( 0.0, lerpArr( result.confidence, hopIdxF ) ) );
        putU64( out.evPayload, (uint64_t)std::llround( se.posFrames ) );
        putF32( out.evPayload, (float)se.residualMs );
        putF32( out.evPayload, (float)conf );
        putU16( out.evPayload, se.region );
        putU16( out.evPayload, (uint16_t)0 );   // flags: reserved (M2+)
    }

    // --- Proposal 40 M3c (Tier B): the "groove.dyn" dynamics payload ------
    // Every quantity is computed on the pendulum's OWN hop grid first (the
    // wrapped phase is never interpolated -- only the derived series are
    // resampled). phi = arg z is the SAME per-hop quantity the section
    // 3.5 counterTension summary reads, so the export and the summary are
    // one physics: whole-run mean(hypot(support,tension))/k reproduces
    // counterTension's own meanF up to resampling (gated in groove_test).
    {
        const std::vector<twGroovePendulumUnitSpec> &specs =
            useTrained ? params.trained.paramsUsed.ensemble
                       : params.pendulum.ensemble;
        const double kPi   = 3.14159265358979323846;
        const double dtSec = pendulumHopFrames / (double)rate;

        // cph/sph are the M3e (v2) PHASE channels: cos/sin of the same phi,
        // computed here on the pendulum's own hop grid and bin-averaged
        // below by the identical lambda. Averaging cos and sin separately
        // IS the circular-mean numerator; a wrapped phase itself may never
        // be bin-averaged as one scalar (twaspects.h's normative doc).
        std::vector<std::vector<double>> sup( nUnits ), ten( nUnits ),
                                          cph( nUnits ), sph( nUnits ),
                                          slp( nUnits ), dis( nUnits ),
                                          cmt( nUnits ), smt( nUnits );
        for( uint32_t u = 0; u < nUnits; u++ ) {
            const twGroovePendulumUnitTrajectory &traj =
                result.unitTrajectories[u];
            const size_t n = traj.phaseWrapped.size();
            sup[u].assign( n, 0.0 ); ten[u].assign( n, 0.0 );
            cph[u].assign( n, 0.0 ); sph[u].assign( n, 0.0 );
            slp[u].assign( n, 0.0 ); dis[u].assign( n, 0.0 );
            cmt[u].assign( n, 0.0 ); smt[u].assign( n, 0.0 );

            // k/alpha mirror runPass1's own derivation (twgroovependulum.cc)
            // -- alpha from the unit's dampingCycles against ITS seeded
            // register, the same fallback for a unit that never seeded.
            const double k             = u < specs.size() ? specs[u].k : 1.5;
            const double dampingCycles = u < specs.size() ? specs[u].dampingCycles : 4.0;
            const double omega0        = traj.omega0;
            const double alpha         = ( dampingCycles > 0.0 && omega0 > 0.0 )
                                           ? -omega0 / ( 2.0 * kPi * dampingCycles )
                                           : -1.0;

            for( size_t t = 0; t < n; t++ ) {
                const double phi = traj.phaseWrapped[t];
                const double F   = traj.driveF[t];
                const double m   = traj.magnitude[t];
                const double cp = std::cos( phi );
                const double sp = std::sin( phi );
                sup[u][t] = k * F * cp;
                ten[u][t] = k * F * sp;
                cph[u][t] = cp;
                sph[u][t] = sp;
                // The METRICAL phase (v3): same two-channel treatment and for
                // the same reason -- a wrapped phase may never be bin-averaged
                // as one scalar.
                const double pm = t < traj.phaseMetric.size() ? traj.phaseMetric[t] : 0.0;
                cmt[u][t] = std::cos( pm );
                smt[u][t] = std::sin( pm );
                dis[u][t] = 2.0 * std::fabs( alpha ) * m * m;
                if( t > 0 && dtSec > 0.0 ) {
                    // Instantaneous phase advance: the per-hop wrapped
                    // difference folded into (-pi, pi] -- valid because one
                    // hop advances well under pi (omega*dt ~ 0.13 rad at
                    // the reference's own rate).
                    double d = phi - traj.phaseWrapped[t - 1];
                    while( d >  kPi ) d -= 2.0 * kPi;
                    while( d <= -kPi ) d += 2.0 * kPi;
                    const double w = traj.omega[t];
                    slp[u][t] = w > 1e-9
                                  ? std::fabs( d / dtSec - w ) / w
                                  : 0.0;
                }
            }
        }

        // BIN-AVERAGED onto the aspect grid, never point-sampled: these are
        // impulsive, power-like series (the drive IS the rectified flux),
        // and a click train's transients land on exactly the pendulum hops
        // a lerp at the coarser grid would sample -- measured on the
        // consistency gate: point-sampling overstated the reference's
        // mean drive by 52 % on a plain 120 BPM click train. A bin mean
        // preserves the mean by construction; an empty bin (an aspect grid
        // FINER than the pendulum's, a non-default envRateHz) falls back to
        // the point sample.
        out.dynRecordCount = nRecs;
        out.dynPayload.reserve( (size_t)nRecs * (size_t)nUnits * 8 * 4 );
        for( uint64_t k = 0; k < nRecs; k++ ) {
            const double binLoF = pendulumHopFrames > 0.0
                ? (double)( k * (uint64_t)out.hopFrames ) / pendulumHopFrames : 0.0;
            const double binHiF = pendulumHopFrames > 0.0
                ? (double)( ( k + 1 ) * (uint64_t)out.hopFrames ) / pendulumHopFrames : 0.0;
            const long lo = (long)binLoF;
            const long hi = (long)binHiF;   // exclusive
            auto emit = [&]( const std::vector<double> &arr ) {
                double v;
                if( hi > lo && !arr.empty() ) {
                    const long a = std::min<long>( std::max<long>( lo, 0 ),
                                                   (long)arr.size() - 1 );
                    const long b = std::min<long>( std::max<long>( hi, a + 1 ),
                                                   (long)arr.size() );
                    double sum = 0.0;
                    for( long t = a; t < b; t++ ) sum += arr[(size_t)t];
                    v = sum / (double)( b - a );
                } else {
                    v = lerpArr( arr, binLoF );
                }
                putF32( out.dynPayload, (float)v );
            };
            for( uint32_t u = 0; u < nUnits; u++ ) {
                // The WIRE order (twaspects.h): support, tension, cosPhi,
                // sinPhi, slip, dissip, then v3's cosMet, sinMet -- APPENDED,
                // so the first six offsets are unchanged.
                emit( sup[u] );
                emit( ten[u] );
                emit( cph[u] );
                emit( sph[u] );
                emit( slp[u] );
                emit( dis[u] );
                emit( cmt[u] );
                emit( smt[u] );
            }
        }
    }

    // Proposal 40 M3: the in-memory-only physical-readout summary (never
    // part of any wire payload above) -- copied straight from `result`,
    // which already computed it as part of pass 2.
    out.counterTension = result.counterTension;
    out.unitMeanR       = result.unitMeanR;

    return out;
}

// ---------------------------------------------------------------------------
// Decoders -- the mirror image of the encoder above, used by the round-trip
// gate (sidecar_test.cc) and by app/test-verb readers.
// ---------------------------------------------------------------------------
std::vector<twGrooveResRecord> twGrooveDecodeResPayload(
    const uint8_t *payload, uint64_t payloadLen, uint32_t nUnits )
{
    std::vector<twGrooveResRecord> out;
    if( payload == nullptr || nUnits == 0 ) return out;
    const uint64_t stride = (uint64_t)( nUnits + 1 ) * 4;
    if( stride == 0 || payloadLen % stride != 0 ) return out;
    const uint64_t n = payloadLen / stride;
    out.reserve( (size_t)n );
    for( uint64_t k = 0; k < n; k++ ) {
        const uint8_t *r = payload + k * stride;
        twGrooveResRecord rec;
        rec.unitPower.resize( nUnits );
        for( uint32_t u = 0; u < nUnits; u++ )
            rec.unitPower[u] = getF32( r + (size_t)u * 4 );
        rec.compliance = getF32( r + (size_t)nUnits * 4 );
        out.push_back( std::move( rec ) );
    }
    return out;
}

std::vector<twGrooveDynRecord> twGrooveDecodeDynPayload(
    const uint8_t *payload, uint64_t payloadLen, uint32_t nUnits )
{
    std::vector<twGrooveDynRecord> out;
    if( payload == nullptr || nUnits == 0 ) return out;
    const uint64_t stride = (uint64_t)nUnits * 8 * 4;
    if( stride == 0 || payloadLen % stride != 0 ) return out;
    const uint64_t n = payloadLen / stride;
    out.reserve( (size_t)n );
    for( uint64_t k = 0; k < n; k++ ) {
        const uint8_t *r = payload + k * stride;
        twGrooveDynRecord rec;
        rec.units.resize( nUnits );
        for( uint32_t u = 0; u < nUnits; u++ ) {
            // 8 floats per unit since v3 -- MUST track the record stride
            // above. These two numbers are the same fact written twice and are
            // exactly what a channel-append gets wrong: with u*24 against a
            // 32-byte unit the channels scramble across units and decode to
            // values outside [-1,1], which is how this was caught.
            const uint8_t *s = r + (size_t)u * 8 * 4;
            rec.units[u].support = getF32( s );
            rec.units[u].tension = getF32( s + 4 );
            rec.units[u].cosPhi  = getF32( s + 8 );
            rec.units[u].sinPhi  = getF32( s + 12 );
            rec.units[u].slip    = getF32( s + 16 );
            rec.units[u].dissip  = getF32( s + 20 );
            rec.units[u].cosMet  = getF32( s + 24 );
            rec.units[u].sinMet  = getF32( s + 28 );
        }
        out.push_back( std::move( rec ) );
    }
    return out;
}

std::vector<twGrooveEvRecord> twGrooveDecodeEvPayload(
    const uint8_t *payload, uint64_t payloadLen )
{
    std::vector<twGrooveEvRecord> out;
    if( payload == nullptr ) return out;
    constexpr uint64_t stride = 20;
    if( payloadLen % stride != 0 ) return out;
    const uint64_t n = payloadLen / stride;
    out.reserve( (size_t)n );
    for( uint64_t k = 0; k < n; k++ ) {
        const uint8_t *r = payload + k * stride;
        twGrooveEvRecord rec;
        rec.pos        = getU64( r );
        rec.residualMs = getF32( r + 8 );
        rec.confidence = getF32( r + 12 );
        rec.region     = getU16( r + 16 );
        rec.flags      = getU16( r + 18 );
        out.push_back( rec );
    }
    return out;
}
