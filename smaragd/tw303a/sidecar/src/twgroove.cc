#include "tw/sidecar/twgroove.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <functional>
#include <utility>

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

// =============================================================================
// Baseline estimator (M0 section 6 spec). See twgroove.h for the algorithm
// summary; this is the normative implementation.
// =============================================================================

namespace {

double medianOf( std::vector<double> v )
{
    if( v.empty() )
        return 0.0;
    std::sort( v.begin(), v.end() );
    const size_t n = v.size();
    return ( n % 2 == 1 ) ? v[n / 2] : 0.5 * ( v[n / 2 - 1] + v[n / 2] );
}

// 1.4826 * median(|x - median(x)|) -- the standard consistent-for-Gaussian
// scale estimator (matches "sigma" for a normal distribution).
double madSigma( const std::vector<double> &v, double med )
{
    if( v.size() < 2 )
        return 0.0;
    std::vector<double> dev;
    dev.reserve( v.size() );
    for( double x : v )
        dev.push_back( std::fabs( x - med ) );
    return 1.4826 * medianOf( dev );
}

// Wrap x into (-half, half].
double wrapSigned( double x, double half )
{
    double y = std::fmod( x + half, 2.0 * half );
    if( y < 0.0 )
        y += 2.0 * half;
    return y - half;
}

// One (near-simultaneous-event) cluster, used for tatum recovery: several
// regions firing on the same nominal beat collapse to one cluster time (the
// mean of member positions) so the IOI histogram is not swamped by ~0 ms
// same-beat gaps between regions.
std::vector<double> clusterEventTimesSec( const std::vector<twGrooveEvent> &events,
                                          double rate, double coincidenceMs )
{
    std::vector<double> out;
    if( events.empty() )
        return out;
    // events is already sorted ascending by posFrames (twGrooveField's
    // contract), but events here is a COPY reference sorted by the caller
    // via posFrames -- confirm by using as given (front end guarantees it).
    double sumT = 0.0;
    int    n    = 0;
    double lastT = events[0].posFrames / rate;
    for( size_t i = 0; i < events.size(); i++ ) {
        const double t = events[i].posFrames / rate;
        if( n > 0 && ( t - lastT ) * 1000.0 > coincidenceMs ) {
            out.push_back( sumT / (double)n );
            sumT = 0.0;
            n    = 0;
        }
        sumT += t;
        n++;
        lastT = t;
    }
    if( n > 0 )
        out.push_back( sumT / (double)n );
    return out;
}

// Step 1, FALLBACK method: recover the tatum period from an ascending list of
// cluster times via an IOI histogram (twGrooveBaselineParams doc). Used only
// when the autocorrelation above cannot run (field too short for the
// candidate lag range).
double recoverTatumPeriodSec( const std::vector<double> &clusterTimesSec,
                              const twGrooveBaselineParams &p )
{
    if( clusterTimesSec.size() < 2 )
        return 0.0;

    std::vector<double> ioisSec;
    ioisSec.reserve( clusterTimesSec.size() );
    for( size_t i = 1; i < clusterTimesSec.size(); i++ ) {
        const double dt = clusterTimesSec[i] - clusterTimesSec[i - 1];
        if( dt >= p.minTatumSec && dt <= p.maxTatumSec )
            ioisSec.push_back( dt );
    }
    if( ioisSec.size() < 2 )
        return 0.0;

    const double binSec = p.tatumHistBinMs / 1000.0;
    const int    nBins  = (int)std::ceil( ( p.maxTatumSec - p.minTatumSec ) / binSec ) + 1;
    std::vector<int> hist( (size_t)std::max( 1, nBins ), 0 );
    for( double v : ioisSec ) {
        int b = (int)( ( v - p.minTatumSec ) / binSec );
        if( b < 0 ) b = 0;
        if( b >= (int)hist.size() ) b = (int)hist.size() - 1;
        hist[(size_t)b]++;
    }
    int bestBin = 0, bestCount = -1;
    for( size_t b = 0; b < hist.size(); b++ )
        if( hist[b] > bestCount ) { bestCount = hist[b]; bestBin = (int)b; }
    const double binCenterSec = p.minTatumSec + ( (double)bestBin + 0.5 ) * binSec;

    const double bandSec = p.tatumRefineBandMs / 1000.0;
    double sum = 0.0;
    int    n   = 0;
    for( double v : ioisSec ) {
        if( std::fabs( v - binCenterSec ) <= bandSec ) { sum += v; n++; }
    }
    if( n == 0 )
        return binCenterSec;
    return sum / (double)n;
}

// One local-window pulse fit: t_e ~= phase0 + tatumLocal * round((t_e -
// idxAnchor)/tatumSeed). Ordinary least squares over (index, t), one outlier
// rejection pass. Returns false (fit left default) if fewer than 2 events
// survive rejection.
//
// idxAnchor is the window's FIRST event time, deliberately NOT its arithmetic
// mean (found the hard way on the AC(a0) fixture, whose windows are exact
// multiples of a whole number of tatums): the mean of an EVEN count of
// evenly-spaced points is always exactly halfway between two grid points, so
// (t-mean)/tatumSeed lands on an exact x.5 for every point in the window, and
// std::round's "round half away from zero" then maps the two halves of the
// window to indices that JUMP by 2 at the window's own midpoint (skipping
// index 0) instead of stepping by 1 -- a discontinuity no single line can
// fit, and the LSQ compromise came out biased by exactly half a tatum period
// (measured: a window's phase0 landing at +0.125 s on a 0.25 s tatum). The
// first event's OWN time is never an artificial midpoint, so anchoring there
// makes every offset (t_e - idxAnchor)/tatumSeed land near a true INTEGER for
// both synthetic-exact and real-noisy material, and the tie case cannot arise.
struct WindowFit {
    double centerSec  = 0.0;
    double phase0Sec   = 0.0;
    double tatumSec    = 0.0;
    bool   valid       = false;
};

bool fitWindow( const std::vector<double> &timesInWindowSec, double tatumSeed,
               double windowCenterSec, double outlierRejectMs, WindowFit &out )
{
    if( timesInWindowSec.size() < 2 || tatumSeed <= 0.0 )
        return false;

    const double idxAnchor = timesInWindowSec.front();

    auto lsqFit = [&]( const std::vector<double> &ts, double &phase0, double &tatum ) -> bool {
        // index_e = round((t_e - idxAnchor)/tatumSeed); fit t = phase0 + tatum*index.
        double sumI = 0.0, sumT = 0.0, sumII = 0.0, sumIT = 0.0;
        int    n    = 0;
        for( double t : ts ) {
            const double idx = std::round( ( t - idxAnchor ) / tatumSeed );
            sumI += idx; sumT += t; sumII += idx * idx; sumIT += idx * t;
            n++;
        }
        if( n < 2 )
            return false;
        const double meanI = sumI / (double)n;
        const double meanTT = sumT / (double)n;
        const double covIT  = sumIT / (double)n - meanI * meanTT;
        const double varI    = sumII / (double)n - meanI * meanI;
        if( varI <= 1e-12 ) {
            // All events landed on the SAME index (a degenerate/too-narrow
            // window) -- fall back to the seed period, phase at the mean.
            tatum  = tatumSeed;
            phase0 = meanTT - meanI * tatum;
            return true;
        }
        tatum  = covIT / varI;
        phase0 = meanTT - meanI * tatum;
        return true;
    };

    double phase0 = 0.0, tatum = tatumSeed;
    if( !lsqFit( timesInWindowSec, phase0, tatum ) )
        return false;
    if( tatum <= 0.0 )
        return false;

    std::vector<double> survivors;
    survivors.reserve( timesInWindowSec.size() );
    for( double t : timesInWindowSec ) {
        const double idx       = std::round( ( t - idxAnchor ) / tatumSeed );
        const double predicted = phase0 + tatum * idx;
        const double residMs   = ( t - predicted ) * 1000.0;
        if( std::fabs( residMs ) <= outlierRejectMs )
            survivors.push_back( t );
    }
    if( survivors.size() < 2 )
        survivors = timesInWindowSec;   // rejection ate everything -- keep the raw fit rather than fail

    double phase0b = phase0, tatumb = tatum;
    lsqFit( survivors, phase0b, tatumb );
    if( tatumb <= 0.0 )
        return false;

    out.centerSec = windowCenterSec;
    out.phase0Sec = phase0b;
    out.tatumSec  = tatumb;
    out.valid     = true;
    return true;
}

} // namespace

// Public: autocorrelation of the summed region flux over an arbitrary lag
// range. See twgroove.h's doc -- this is the primitive twGrooveBaselineAnalyze
// uses internally for tatum recovery (PRIMARY method, replacing a pure IOI
// histogram: measured on AC (g)'s wash fixture, a continuous broadband
// texture triples the picked EVENT count, and every extra pick injects a
// spurious inter-onset interval into a histogram -- the histogram's peak bin
// moved from the true 250 ms tatum to 115 ms, corrupting every downstream
// residual. The autocorrelation is computed over the CONTINUOUS flux field
// rather than over picked events, so a wash's own texture -- which has no
// strong periodic structure at the tatum's own lag -- does not inject
// discrete false candidates the way it injects false events), and is exposed
// publicly because twgroovependulum.cc needs a SECOND instance of it at a
// slower lag range to seed its bar-scale units (see twgroove.h's doc on this
// function for why a fixed tatum-multiple is not safe to assume).
double twGrooveRecoverPeriodByAutocorrelation( const twGrooveField &field,
                                               double minPeriodSec, double maxPeriodSec )
{
    if( field.nHops == 0 || field.rate == 0 || field.hopFrames == 0 )
        return 0.0;
    if( !( minPeriodSec > 0.0 ) || !( maxPeriodSec > minPeriodSec ) )
        return 0.0;
    const uint32_t nRegions = (uint32_t)field.regionLowHz.size();
    if( nRegions == 0 || field.regionFlux.size() < (size_t)nRegions * (size_t)field.nHops )
        return 0.0;

    std::vector<double> drive( field.nHops, 0.0 );
    for( uint32_t r = 0; r < nRegions; r++ ) {
        const float *flux = &field.regionFlux[(size_t)r * (size_t)field.nHops];
        for( uint32_t k = 0; k < field.nHops; k++ ) drive[k] += (double)flux[k];
    }
    double mean = 0.0;
    for( double v : drive ) mean += v;
    mean /= (double)drive.size();
    for( double &v : drive ) v -= mean;

    const double hopSec = (double)field.hopFrames / (double)field.rate;
    const int minLag = std::max( 1, (int)std::round( minPeriodSec / hopSec ) );
    const int maxLag = std::min( (int)field.nHops - 1, (int)std::round( maxPeriodSec / hopSec ) );
    if( maxLag <= minLag )
        return 0.0;

    // Raw (unnormalized) sum -- tried normalizing by the per-lag overlap
    // count (the "unbiased" estimator) and it corrupted the fine TATUM
    // search: the shrinking overlap at large lag inflates that estimate's
    // own variance, and on fixture (a0) it picked a noisy large-lag peak
    // over the true, well-conditioned 0.25 s one (recovered 0.5 s instead).
    // Normalizing by the fixed total length changes nothing (a constant
    // divisor cannot change which lag is the argmax) so it is not tried.
    auto autocorrAt = [&]( int lag ) -> double {
        double s = 0.0;
        for( uint32_t k = 0; (int)k + lag < (int)field.nHops; k++ ) s += drive[k] * drive[k + (uint32_t)lag];
        return s;
    };

    // The plain global argmax (not a "first strong peak" search): tried that
    // as an octave/harmonic-error guard while this function was still also
    // used for the M0 ensemble's bar-scale seeding, and it measurably hurt
    // the ONE caller left after that seeding moved to a fixed absolute
    // anchor (twGroovePendulumParams::defaultBarPeriodSec's doc) -- the
    // fine-grained TATUM search, where AC (i)'s 2 ms bound is tight enough
    // that trading the sharpest peak for "first past 75% of it" measurably
    // moved the recovered tatum on a short (16 s) slice and turned a passing
    // phantom-shift measurement into a 10+ ms one.
    int    bestLag = -1;
    double bestVal = -1e300;
    for( int lag = minLag; lag <= maxLag; lag++ ) {
        const double v = autocorrAt( lag );
        if( v > bestVal ) { bestVal = v; bestLag = lag; }
    }
    if( bestLag < 0 )
        return 0.0;

    const double y0 = ( bestLag > minLag ) ? autocorrAt( bestLag - 1 ) : bestVal;
    const double y1 = bestVal;
    const double y2 = ( bestLag < maxLag ) ? autocorrAt( bestLag + 1 ) : bestVal;
    const double denom = y0 - 2.0 * y1 + y2;
    double       delta = ( std::fabs( denom ) > 1e-12 ) ? 0.5 * ( y0 - y2 ) / denom : 0.0;
    if( delta > 0.5 ) delta = 0.5;
    if( delta < -0.5 ) delta = -0.5;
    return ( (double)bestLag + delta ) * hopSec;
}

// Public: shared per-region pooling for BOTH estimators (twgroove.h's doc).
// Bleed gate, then mu/drift/LOCAL-median sigma/bimodality -- see
// twGrooveRegionStats' doc for the exact definitions.
twGrooveResidualReport twGroovePoolRegionStats(
    const std::vector<std::vector<twGrooveScoredEvent>> &eventsByRegion,
    double totalSec, const twGrooveStatsParams &stats )
{
    const uint32_t nRegions = (uint32_t)eventsByRegion.size();
    twGrooveResidualReport report;
    report.perRegion.assign( nRegions, twGrooveRegionStats{} );

    for( uint32_t r = 0; r < nRegions; r++ ) {
        // --- Bleed gate: reference level = median amplitude of the top
        // (loudest) half of this region's OWN events, sorted descending.
        // Events more than bleedGateDb below it are dropped BEFORE anything
        // else is computed -- see twGrooveStatsParams::bleedGateDb's doc. ---
        std::vector<twGrooveScoredEvent> gated;
        {
            const std::vector<twGrooveScoredEvent> &raw = eventsByRegion[r];
            if( !raw.empty() ) {
                std::vector<float> ampsDesc;
                ampsDesc.reserve( raw.size() );
                for( const auto &e : raw ) ampsDesc.push_back( e.amp );
                std::sort( ampsDesc.begin(), ampsDesc.end(), std::greater<float>() );
                const size_t topHalfN = std::max<size_t>( 1, ampsDesc.size() / 2 );
                std::vector<float> topHalf( ampsDesc.begin(), ampsDesc.begin() + (long)topHalfN );
                std::sort( topHalf.begin(), topHalf.end() );
                const float refLevel = topHalf[topHalf.size() / 2];   // median of the top half

                if( refLevel > 0.0f ) {
                    const double thresholdLinear =
                        (double)refLevel * std::pow( 10.0, -stats.bleedGateDb / 20.0 );
                    for( const auto &e : raw )
                        if( (double)e.amp >= thresholdLinear )
                            gated.push_back( e );
                } else {
                    gated = raw;   // degenerate (all-zero amplitude) -- gate is a no-op
                }
            }
        }

        twGrooveRegionStats &out = report.perRegion[r];
        if( gated.empty() )
            continue;   // ALL bleed (or no events at all) -- correctly hasData=false
        out.hasData = true;
        out.nEvents = (int)gated.size();

        std::vector<double> res;
        res.reserve( gated.size() );
        for( const auto &e : gated ) res.push_back( e.residualMs );
        out.muMs = medianOf( res );

        // --- Bimodality: on the RAW (gated) residuals, never detrended --
        // a stable two-cluster split is a different phenomenon from local
        // drift (twGrooveRegionStats' own doc). ---
        {
            double lo = res[0], hi = res[0];
            for( double v : res ) { lo = std::min( lo, v ); hi = std::max( hi, v ); }
            const double span  = std::max( 1e-6, hi - lo );
            const double binMs = 2.0;
            const int    nBins = (int)std::ceil( span / binMs ) + 1;
            std::vector<int> hist( (size_t)std::max( 1, nBins ), 0 );
            for( double v : res ) {
                int b = (int)( ( v - lo ) / binMs );
                if( b < 0 ) b = 0;
                if( b >= (int)hist.size() ) b = (int)hist.size() - 1;
                hist[(size_t)b]++;
            }
            int peakA = -1, peakB = -1, cA = -1, cB = -1;
            for( int b = 0; b < (int)hist.size(); b++ ) {
                if( hist[b] > cA ) { cB = cA; peakB = peakA; cA = hist[b]; peakA = b; }
                else if( hist[b] > cB ) { cB = hist[b]; peakB = b; }
            }
            if( peakA >= 0 && peakB >= 0 ) {
                const double centerA = lo + ( (double)peakA + 0.5 ) * binMs;
                const double centerB = lo + ( (double)peakB + 0.5 ) * binMs;
                if( std::fabs( centerA - centerB ) >= stats.bimodalMinGapMs ) {
                    double sumA = 0.0, sumB = 0.0; int nA = 0, nB = 0;
                    for( double v : res ) {
                        if( std::fabs( v - centerA ) <= std::fabs( v - centerB ) ) { sumA += v; nA++; }
                        else { sumB += v; nB++; }
                    }
                    const double fracA = (double)nA / (double)res.size();
                    const double fracB = (double)nB / (double)res.size();
                    if( fracA >= stats.bimodalMinFrac && fracB >= stats.bimodalMinFrac ) {
                        out.bimodal = true;
                        out.modeAMs = ( nA > 0 ) ? sumA / (double)nA : centerA;
                        out.modeBMs = ( nB > 0 ) ? sumB / (double)nB : centerB;
                        if( out.modeAMs > out.modeBMs ) std::swap( out.modeAMs, out.modeBMs );
                    }
                }
            }
        }

        // --- Windowed drift trace (on the gated residuals). ---
        if( stats.driftStepSec > 0.0 && stats.driftWindowSec > 0.0 ) {
            for( double wStart = 0.0; wStart < totalSec; wStart += stats.driftStepSec ) {
                const double wEnd = std::min( wStart + stats.driftWindowSec, totalSec );
                std::vector<double> inWin;
                for( const auto &e : gated )
                    if( e.tSec >= wStart && e.tSec < wEnd )
                        inWin.push_back( e.residualMs );
                if( !inWin.empty() )
                    out.drift.push_back( { wStart + 0.5 * ( wEnd - wStart ), medianOf( inWin ) } );
                if( wEnd >= totalSec )
                    break;
            }
        }

        // --- sigma: spread around the LOCAL (drift-trace) median, not the
        // global one (twGrooveRegionStats::sigmaMs's doc). Falls back to the
        // global-median MAD when fewer than 2 drift windows exist. ---
        if( out.drift.size() >= 2 ) {
            std::vector<double> detrended;
            detrended.reserve( gated.size() );
            for( const auto &e : gated ) {
                size_t bestIdx = 0;
                double bestD   = 1e18;
                for( size_t i = 0; i < out.drift.size(); i++ ) {
                    const double d = std::fabs( out.drift[i].tSec - e.tSec );
                    if( d < bestD ) { bestD = d; bestIdx = i; }
                }
                detrended.push_back( e.residualMs - out.drift[bestIdx].muMs );
            }
            // Spread around ZERO, not re-centered: each value is already
            // relative to its OWN local window's median, so a second
            // centering would just re-introduce a global bias the local
            // detrend was built to remove.
            out.sigmaMs = madSigma( detrended, 0.0 );
        } else {
            out.sigmaMs = madSigma( res, out.muMs );
        }
    }

    return report;
}

twGrooveBaselineResult twGrooveBaselineAnalyze( const twGrooveField &field,
                                                const twGrooveBaselineParams &p )
{
    twGrooveBaselineResult result;
    if( field.events.empty() || field.nHops == 0 || field.rate == 0 )
        return result;

    const double rate = (double)field.rate;
    const double totalSec = (double)field.nHops * (double)field.hopFrames / rate;

    // --- Step 1: tatum period. Autocorrelation of the summed region flux is
    // PRIMARY (see twGrooveRecoverPeriodByAutocorrelation's doc for why);
    // the IOI histogram over the merged (clustered) event train is the
    // FALLBACK for material too short to cover the candidate lag range. ---
    const std::vector<double> clusterTimesSec =
        clusterEventTimesSec( field.events, rate, p.coincidenceMs );
    result.tatumPeriodSec =
        twGrooveRecoverPeriodByAutocorrelation( field, p.minTatumSec, p.maxTatumSec );
    if( result.tatumPeriodSec <= 0.0 )
        result.tatumPeriodSec = recoverTatumPeriodSec( clusterTimesSec, p );
    if( result.tatumPeriodSec <= 0.0 )
        return result;   // no usable pulse -- honest empty result, not a guess

    // --- Step 2: sliding-window local pulse fits over the CLUSTER train. ---
    // (Fitting the clusters, not the raw per-region events, keeps one fit per
    // beat rather than letting a beat with more populated regions outvote a
    // beat with fewer in the least-squares sum.)
    std::vector<WindowFit> windows;
    if( p.stepSec > 0.0 && p.windowSec > 0.0 ) {
        for( double wStart = 0.0; wStart < totalSec; wStart += p.stepSec ) {
            const double wEnd = std::min( wStart + p.windowSec, totalSec );
            std::vector<double> inWindow;
            for( double t : clusterTimesSec )
                if( t >= wStart && t < wEnd )
                    inWindow.push_back( t );
            WindowFit fit;
            if( fitWindow( inWindow, result.tatumPeriodSec, wStart + 0.5 * ( wEnd - wStart ),
                           p.outlierRejectMs, fit ) )
                windows.push_back( fit );
            if( wEnd >= totalSec )
                break;
        }
    }
    if( windows.empty() ) {
        // Degenerate (very short material, or nothing survived a fit): fall
        // back to ONE global fit over every cluster time.
        WindowFit fit;
        if( fitWindow( clusterTimesSec, result.tatumPeriodSec, 0.5 * totalSec,
                       p.outlierRejectMs, fit ) )
            windows.push_back( fit );
        else
            return result;
    }

    auto nearestWindow = [&]( double tSec ) -> const WindowFit & {
        size_t best = 0;
        double bestD = 1e18;
        for( size_t i = 0; i < windows.size(); i++ ) {
            const double d = std::fabs( windows[i].centerSec - tSec );
            if( d < bestD ) { bestD = d; best = i; }
        }
        return windows[best];
    };

    // --- Step 3: per-event residual against the nearest window's fit, ------
    // scored (with amplitude) per region, then pooled by the SHARED function
    // (bleed gate, mu, drift, local-median sigma, bimodality -- twgroove.h's
    // twGroovePoolRegionStats doc).
    const uint32_t nRegions = (uint32_t)( field.regionLowHz.size() );
    std::vector<std::vector<twGrooveScoredEvent>> eventsByRegion( nRegions );

    for( const twGrooveEvent &ev : field.events ) {
        if( ev.region >= nRegions )
            continue;
        const double t = ev.posFrames / rate;
        const WindowFit &w = nearestWindow( t );
        if( !w.valid || w.tatumSec <= 0.0 )
            continue;
        const double idx       = std::round( ( t - w.phase0Sec ) / w.tatumSec );
        const double predicted = w.phase0Sec + w.tatumSec * idx;
        const double residMs   = wrapSigned( ( t - predicted ) * 1000.0, w.tatumSec * 500.0 );
        eventsByRegion[ev.region].push_back( { t, residMs, ev.amp } );
    }

    result.residuals = twGroovePoolRegionStats( eventsByRegion, totalSec, p.stats );
    return result;
}
