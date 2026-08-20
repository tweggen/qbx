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

    const twGroovePendulumResult result =
        twGroovePendulumAnalyze( field, params.pendulum );
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
