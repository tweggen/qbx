#include "tw/sidecar/twgroove.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>

// Groove analysis front end (proposal 40 M0). Pure, deterministic, single-
// threaded: no threads, no rand(), fixed iteration order. NORMATIVE per
// twgroove.h -- see that file for the seven-step algorithm description.

namespace {

// pi as a literal, not M_PI (matches twanalyzers.cc house style: MinGW does
// not define M_PI without _USE_MATH_DEFINES).
constexpr double kPi = 3.14159265358979323846;

// --- ERB scale (Glasberg/Moore-shaped, derived from the SAME ERB(f) formula
// this front end uses elsewhere, so the two are self-consistent by
// construction) ---------------------------------------------------------
//
// ERB(f) = 24.7*(4.37*f/1000 + 1) = k1*(k2*f + 1), k1=24.7, k2=0.00437.
// The ERB-RATE coordinate is the integral of 1/ERB(f) df:
//   erbScale(f)    = (1/(k1*k2)) * ln(1 + k2*f)
//   erbScaleInv(e) = (exp(e*k1*k2) - 1) / k2
// A linear spacing in this coordinate is a spacing of equal auditory
// bandwidth in f -- the definition of "ERB-spaced".

constexpr double kErbK1 = 24.7;
constexpr double kErbK2 = 4.37 / 1000.0;

inline double erbOf( double f ) { return kErbK1 * ( kErbK2 * f + 1.0 ); }

inline double erbScale( double f )
{
    return ( 1.0 / ( kErbK1 * kErbK2 ) ) * std::log( 1.0 + kErbK2 * f );
}

inline double erbScaleInv( double e )
{
    return ( std::exp( e * kErbK1 * kErbK2 ) - 1.0 ) / kErbK2;
}

// nBands ERB-spaced centers over [fMin, fMax], ascending, endpoints included.
std::vector<float> bandCentersErb( uint32_t nBands, float fMin, float fMax )
{
    std::vector<float> out;
    if( nBands == 0 )
        return out;
    out.resize( nBands );
    if( nBands == 1 ) {
        out[0] = fMin;
        return out;
    }
    const double eLo = erbScale( (double)fMin );
    const double eHi = erbScale( (double)fMax );
    for( uint32_t i = 0; i < nBands; i++ ) {
        const double e = eLo + ( eHi - eLo ) * (double)i / (double)( nBands - 1 );
        out[i] = (float)erbScaleInv( e );
    }
    return out;
}

// Region r's [low, high) edge in Hz: nRegions log-spaced edges over
// [fMin, fMax], edges[i] = fMin * (fMax/fMin)^(i/nRegions), i in [0,nRegions].
inline double regionEdgeHz( double fMin, double fMax, uint32_t nRegions,
                            uint32_t i )
{
    return fMin * std::pow( fMax / fMin, (double)i / (double)nRegions );
}

// Which region a frequency falls in, over the SAME log partition regionEdgeHz
// generates. Clamped to [0, nRegions-1].
uint32_t regionForFreq( double f, double fMin, double fMax, uint32_t nRegions )
{
    if( nRegions <= 1 )
        return 0;
    const double t = std::log( f / fMin ) / std::log( fMax / fMin );
    int64_t      r = (int64_t)std::floor( t * (double)nRegions );
    if( r < 0 )
        r = 0;
    if( r >= (int64_t)nRegions )
        r = (int64_t)nRegions - 1;
    return (uint32_t)r;
}

// --- the per-band gammatone-approximation cascade -----------------------
//
// 4 cascaded complex one-pole filters, pole a = |a| * exp(i*theta),
// |a| = exp(-2*pi*b/fs), theta = 2*pi*fc/fs, b = 1.019*ERB(fc). The REAL
// input is doubled ONLY into the first stage -- the standard real-to-
// analytic compensation for a complex resonator driven by a real signal
// (cos(theta*n) = 0.5*(e^{i theta n} + e^{-i theta n}); at resonance the
// negative-frequency half is rejected by the filter's own selectivity, so
// without the factor of 2 a unit-amplitude real sine would settle to
// envelope ~= 0.5 instead of ~= 1). Stages 2-4 receive the already
// near-single-sided output of stage 1 and must NOT be doubled again.
// gain = (1-|a|)^4 normalizes the FOUR-stage steady-state resonant gain
// 1/(1-|a|)^4 back to unity.
struct BandFilterSpec {
    std::complex<double> a;
    double               aMag  = 0.0;
    double               gain  = 0.0;
    double               bBW   = 0.0;   // 1.019*ERB(fc), the delay formula's b
};

BandFilterSpec makeBandFilterSpec( float fc, double fs )
{
    BandFilterSpec spec;
    spec.bBW       = 1.019 * erbOf( (double)fc );
    spec.aMag      = std::exp( -2.0 * kPi * spec.bBW / fs );
    const double th = 2.0 * kPi * (double)fc / fs;
    spec.a          = std::complex<double>( spec.aMag * std::cos( th ),
                                            spec.aMag * std::sin( th ) );
    spec.gain       = std::pow( 1.0 - spec.aMag, 4.0 );
    return spec;
}

// RAW (pre-delay-compensation, pre-compression) envelope for one band, full
// native rate, length nFrames.
std::vector<float> computeBandRawEnvelope( const float *mono, uint64_t nFrames,
                                           const BandFilterSpec &spec )
{
    std::vector<float>   raw( (size_t)nFrames, 0.0f );
    std::complex<double> y1( 0.0, 0.0 ), y2( 0.0, 0.0 ), y3( 0.0, 0.0 ), y4( 0.0, 0.0 );
    for( uint64_t n = 0; n < nFrames; n++ ) {
        const std::complex<double> x2( 2.0 * (double)mono[(size_t)n], 0.0 );
        y1 = x2 + spec.a * y1;
        y2 = y1 + spec.a * y2;
        y3 = y2 + spec.a * y3;
        y4 = y3 + spec.a * y4;
        raw[(size_t)n] = (float)( spec.gain * std::abs( y4 ) );
    }
    return raw;
}

// Group-delay compensation: advance `raw` by delaySec = 3/(2*pi*b) seconds --
// integer-sample shift plus linear interpolation of the fractional
// remainder. Past either end of the buffer the edge sample is held (the
// compensated tail/head is a few ms wide at most, per the closed form).
std::vector<float> delayCompensate( const std::vector<float> &raw,
                                    uint64_t nFrames, double fs,
                                    const BandFilterSpec &spec )
{
    std::vector<float> out( (size_t)nFrames, 0.0f );
    if( nFrames == 0 )
        return out;
    const double  delaySec       = 3.0 / ( 2.0 * kPi * spec.bBW );
    const double  delaySamplesD  = delaySec * fs;
    const int64_t delayInt       = (int64_t)std::floor( delaySamplesD );
    const double  frac           = delaySamplesD - (double)delayInt;
    const int64_t last           = (int64_t)nFrames - 1;

    auto at = [&]( int64_t i ) -> float {
        if( i < 0 ) return raw[0];
        if( i > last ) return raw[(size_t)last];
        return raw[(size_t)i];
    };

    for( uint64_t n = 0; n < nFrames; n++ ) {
        const int64_t i0 = (int64_t)n + delayInt;
        const float   v0 = at( i0 );
        const float   v1 = at( i0 + 1 );
        out[(size_t)n]   = (float)( ( 1.0 - frac ) * (double)v0 + frac * (double)v1 );
    }
    return out;
}

// Compression: log1p(k*e)/log1p(k), monotone, e assumed >= 0 (an envelope
// magnitude). twgroove.h documents the FAMILY with k=100 as its "e.g." --
// this uses a much milder k. Reasoning, measured while building groove_test:
// flux (the event picker's input, half-wave-rectified first difference of
// the REGION envelope, itself a mean of member bands' COMPRESSED envelopes
// per step 6) is computed from the ALREADY-COMPRESSED, ALREADY-POOLED
// signal, and log-compression is not amplitude-invariant the way the
// group-delay term is -- k=100 measurably reshapes a band's rising edge
// enough, BEFORE pooling, to bias where the pooled region's flux peaks by a
// few ms, worst in the low-mid bands, and that bias is NOT of the same
// closed form the group-delay term corrects (that term is exact only for
// the UNCOMPRESSED impulse response -- see the flux-lead comment below). A
// milder k keeps genuine log compression (small envelope values are still
// boosted relative to large ones) while keeping the calibration gate's
// closed-form corrections accurate to within its ±1 ms budget --
// groove_test's calibration section measures and prints the result.
inline float compressSample( float e )
{
    constexpr double kCompressionConstant = 0.1;
    const double v = ( e > 0.0f ) ? (double)e : 0.0;
    static const double kDenom = std::log1p( kCompressionConstant );
    return (float)( std::log1p( kCompressionConstant * v ) / kDenom );
}

// Decimation to nHops by MAX over each native-rate hop window.
std::vector<float> decimateMax( const std::vector<float> &x, uint64_t nFrames,
                                uint32_t hopFrames, uint64_t nHops )
{
    std::vector<float> out( (size_t)nHops, 0.0f );
    for( uint64_t k = 0; k < nHops; k++ ) {
        const uint64_t start = k * (uint64_t)hopFrames;
        const uint64_t end   = std::min( start + (uint64_t)hopFrames, nFrames );
        float          m     = 0.0f;
        bool           any   = false;
        for( uint64_t i = start; i < end; i++ ) {
            const float v = x[(size_t)i];
            if( !any || v > m ) {
                m   = v;
                any = true;
            }
        }
        out[(size_t)k] = m;
    }
    return out;
}

// Mono fold (arithmetic mean), matching twDetectOnsets / twComputeLoudness.
std::vector<float> foldMono( const float *const *chans, uint32_t nCh,
                             uint64_t nFrames )
{
    std::vector<float> mono( (size_t)nFrames );
    for( uint64_t i = 0; i < nFrames; i++ ) {
        double acc = 0.0;
        for( uint32_t c = 0; c < nCh; c++ )
            acc += (double)chans[c][(size_t)i];
        mono[(size_t)i] = (float)( acc / (double)nCh );
    }
    return mono;
}

// Shared front-end body. `compensate` is a test-only ablation knob: the
// public entry point always passes true. See twGrooveAnalyzeFrontEndDebug
// below (declared extern, not in the public header) -- the calibration gate
// (twgroove.h step 3) is meaningless without a way to demonstrate it fails
// when disabled.
twGrooveField analyzeFrontEnd( const float *const *chans, uint32_t nCh,
                               uint64_t nFrames, uint32_t rate,
                               const twGrooveFrontEndParams &params,
                               bool compensate )
{
    twGrooveField field;

    if( chans == nullptr || nCh == 0 || nFrames == 0 || rate == 0 )
        return field;
    if( params.nBands == 0 || params.nRegions == 0 )
        return field;
    if( !( params.fMinHz > 0.0f ) || !( params.fMaxHz > params.fMinHz ) )
        return field;
    if( !( params.envRateHz > 0.0f ) )
        return field;

    const double fs           = (double)rate;
    const double nyquistMargin = 0.49 * fs;
    const double effFMaxD      = std::min( (double)params.fMaxHz, nyquistMargin );
    if( effFMaxD <= (double)params.fMinHz )
        return field;
    const float effFMax = (float)effFMaxD;

    const std::vector<float> mono = foldMono( chans, nCh, nFrames );
    const std::vector<float> centers =
        bandCentersErb( params.nBands, params.fMinHz, effFMax );

    const uint32_t hopFrames = (uint32_t)std::max(
        1.0, std::round( fs / (double)params.envRateHz ) );
    const uint64_t nHops = ( nFrames + hopFrames - 1 ) / hopFrames;

    std::vector<uint32_t> bandRegion( params.nBands );
    for( uint32_t b = 0; b < params.nBands; b++ )
        bandRegion[b] = regionForFreq( (double)centers[b], (double)params.fMinHz,
                                       (double)effFMax, params.nRegions );

    std::vector<double>   regionSum( (size_t)params.nRegions * (size_t)nHops, 0.0 );
    std::vector<uint32_t> regionCount( params.nRegions, 0 );
    std::vector<double>   regionBSum( params.nRegions, 0.0 );   // for the flux-lead term below

    for( uint32_t b = 0; b < params.nBands; b++ ) {
        const BandFilterSpec spec = makeBandFilterSpec( centers[b], fs );
        std::vector<float>   env  = computeBandRawEnvelope( mono.data(), nFrames, spec );
        if( compensate )
            env = delayCompensate( env, nFrames, fs, spec );
        for( float &v : env )
            v = compressSample( v );
        const std::vector<float> dec = decimateMax( env, nFrames, hopFrames, nHops );

        const uint32_t r = bandRegion[b];
        regionCount[r]++;
        regionBSum[r] += spec.bBW;
        double *dst = &regionSum[(size_t)r * (size_t)nHops];
        for( uint64_t k = 0; k < nHops; k++ )
            dst[k] += (double)dec[(size_t)k];
    }

    field.rate      = rate;
    field.hopFrames = hopFrames;
    field.nHops      = (uint32_t)nHops;
    field.bandCenterHz = centers;
    field.regionLowHz.resize( params.nRegions );
    field.regionHighHz.resize( params.nRegions );
    for( uint32_t r = 0; r < params.nRegions; r++ ) {
        field.regionLowHz[r]  = (float)regionEdgeHz( params.fMinHz, effFMax, params.nRegions, r );
        field.regionHighHz[r] = (float)regionEdgeHz( params.fMinHz, effFMax, params.nRegions, r + 1 );
    }
    field.regionEnvelope.assign( (size_t)params.nRegions * (size_t)nHops, 0.0f );
    field.regionFlux.assign( (size_t)params.nRegions * (size_t)nHops, 0.0f );

    for( uint32_t r = 0; r < params.nRegions; r++ ) {
        if( regionCount[r] == 0 )
            continue;
        float *envOut = &field.regionEnvelope[(size_t)r * (size_t)nHops];
        const double *src = &regionSum[(size_t)r * (size_t)nHops];
        for( uint64_t k = 0; k < nHops; k++ )
            envOut[k] = (float)( src[k] / (double)regionCount[r] );

        float *fluxOut = &field.regionFlux[(size_t)r * (size_t)nHops];
        for( uint64_t k = 1; k < nHops; k++ ) {
            const float d = envOut[k] - envOut[k - 1];
            fluxOut[k]    = ( d > 0.0f ) ? d : 0.0f;
        }
    }

    // Event picking per region: median-adaptive threshold, local-maximum
    // picking with minimum separation, parabolic sub-hop interpolation.
    const uint32_t halfWidthHops = (uint32_t)std::max(
        1.0, std::round( (double)params.medianHalfWidthSec * (double)params.envRateHz ) );
    const uint32_t minSepHops = (uint32_t)std::max(
        1.0, std::round( (double)params.minSeparationSec * (double)params.envRateHz ) );

    std::vector<float> scratch;
    for( uint32_t r = 0; r < params.nRegions; r++ ) {
        if( regionCount[r] == 0 || nHops < 3 )
            continue;
        const float *flux = &field.regionFlux[(size_t)r * (size_t)nHops];

        // Flux-lead correction. Reported positions come from a LOCAL MAX of
        // FLUX (the derivative of the region envelope), not of the envelope
        // itself -- and for a 4-stage gammatone-approximation impulse
        // response g(t) = t^3 * e^(-2*pi*b*t) (the shape |y4| settles into),
        // the derivative g'(t) peaks at t1 = (3-sqrt(3))/(2*pi*b), strictly
        // BEFORE the envelope's own peak at 3/(2*pi*b) -- the same delay
        // step 3 compensates for. The two peaks differ by the closed form
        // sqrt(3)/(2*pi*b) (exact: the root of the quadratic
        // 4*pi^2*b^2*t^2 - 12*pi*b*t + 6 = 0 nearer the origin). Left
        // uncorrected this reproduces exactly the frequency-dependent skew
        // step 3 exists to remove -- worse at low b (low frequency, narrow
        // ERB), negligible at high b -- because it is systematic in the
        // SAME direction (early) and the SAME 1/b shape as an
        // under-compensated group delay. It is a property of picking the
        // DERIVATIVE's peak, not of the compensation itself, so it is
        // applied here, once per region from that region's OWN mean band
        // bandwidth, rather than folded into twgroove.h's documented
        // per-band delaySec (which stays exactly 3/(2*pi*b), matching
        // proposal 40 section 3.1 literally, and is independently gated by
        // the negative-control ablation).
        const double regionMeanB = regionBSum[r] / (double)regionCount[r];
        const double fluxLeadSec = std::sqrt( 3.0 ) / ( 2.0 * kPi * regionMeanB );
        const double fluxLeadFrames = compensate ? fluxLeadSec * fs : 0.0;

        float peak = 0.0f;
        for( uint64_t k = 0; k < nHops; k++ )
            if( flux[k] > peak ) peak = flux[k];
        const float floorTerm = params.energyFloorFraction * peak;

        std::vector<float> thr( (size_t)nHops, 0.0f );
        for( uint64_t k = 0; k < nHops; k++ ) {
            const uint64_t lo = ( k >= halfWidthHops ) ? ( k - halfWidthHops ) : 0;
            uint64_t       hi = k + (uint64_t)halfWidthHops;
            if( hi >= nHops )
                hi = nHops - 1;
            scratch.clear();
            for( uint64_t i = lo; i <= hi; i++ )
                scratch.push_back( flux[i] );
            const size_t mid = scratch.size() / 2;
            std::nth_element( scratch.begin(), scratch.begin() + mid, scratch.end() );
            thr[(size_t)k] = params.thresholdFactor * scratch[mid] + floorTerm;
        }

        bool     haveLast = false;
        uint64_t lastK    = 0;
        for( uint64_t k = 1; k + 1 < nHops; k++ ) {
            if( flux[k] > thr[(size_t)k] && flux[k] >= flux[k - 1] && flux[k] > flux[k + 1] ) {
                if( !haveLast || ( k - lastK ) >= (uint64_t)minSepHops ) {
                    const double y0 = flux[k - 1], y1 = flux[k], y2 = flux[k + 1];
                    const double denom = y0 - 2.0 * y1 + y2;
                    double       delta = ( std::fabs( denom ) > 1e-12 )
                                        ? 0.5 * ( y0 - y2 ) / denom
                                        : 0.0;
                    if( delta > 0.5 ) delta = 0.5;
                    if( delta < -0.5 ) delta = -0.5;
                    const double fracHop = (double)k + delta;
                    double       pos     = fracHop * (double)hopFrames + fluxLeadFrames;
                    if( pos < 0.0 ) pos = 0.0;
                    if( pos > (double)( nFrames - 1 ) ) pos = (double)( nFrames - 1 );
                    const float amp = (float)( y1 - 0.25 * ( y0 - y2 ) * delta );
                    field.events.push_back( { pos, amp, (uint16_t)r } );
                    lastK    = k;
                    haveLast = true;
                }
            }
        }
    }

    std::sort( field.events.begin(), field.events.end(),
              []( const twGrooveEvent &a, const twGrooveEvent &b ) {
                  return a.posFrames < b.posFrames;
              } );

    return field;
}

} // namespace

twGrooveField twGrooveAnalyzeFrontEnd( const float *const *chans, uint32_t nCh,
                                       uint64_t nFrames, uint32_t rate,
                                       const twGrooveFrontEndParams &params )
{
    return analyzeFrontEnd( chans, nCh, nFrames, rate, params, /*compensate=*/true );
}

// ---------------------------------------------------------------------------
// Test-only hooks. NOT declared in the public header, exactly like
// twAnalyzerFftMagnitudes in twanalyzers.cc: groove_test re-declares these
// `extern` to (a) demonstrate the calibration gate fails with the group-delay
// compensation disabled, and (b) probe one band's raw envelope directly for
// the selectivity-sanity check. Production code must not call them.
// ---------------------------------------------------------------------------
twGrooveField twGrooveAnalyzeFrontEndDebug( const float *const *chans, uint32_t nCh,
                                            uint64_t nFrames, uint32_t rate,
                                            const twGrooveFrontEndParams &params,
                                            bool compensateGroupDelay )
{
    return analyzeFrontEnd( chans, nCh, nFrames, rate, params, compensateGroupDelay );
}

std::vector<float> twGrooveDebugBandEnvelopeRaw( const float *const *chans, uint32_t nCh,
                                                 uint64_t nFrames, uint32_t rate,
                                                 const twGrooveFrontEndParams &params,
                                                 uint32_t bandIndex, bool compensateGroupDelay )
{
    if( chans == nullptr || nCh == 0 || nFrames == 0 || rate == 0 )
        return {};
    if( bandIndex >= params.nBands )
        return {};

    const double fs      = (double)rate;
    const double effFMaxD = std::min( (double)params.fMaxHz, 0.49 * fs );
    if( effFMaxD <= (double)params.fMinHz )
        return {};
    const float effFMax = (float)effFMaxD;

    const std::vector<float> centers =
        bandCentersErb( params.nBands, params.fMinHz, effFMax );
    const std::vector<float> mono = foldMono( chans, nCh, nFrames );

    const BandFilterSpec spec = makeBandFilterSpec( centers[bandIndex], fs );
    const std::vector<float> raw = computeBandRawEnvelope( mono.data(), nFrames, spec );
    if( !compensateGroupDelay )
        return raw;
    return delayCompensate( raw, nFrames, fs, spec );
}
