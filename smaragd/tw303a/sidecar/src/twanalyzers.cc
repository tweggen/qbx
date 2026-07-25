#include "tw/sidecar/twanalyzers.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

// Offline per-time analyzers (proposal 27 M1). Pure, deterministic, single-
// threaded: no threads, no rand(), fixed iteration order, no fast-math tricks.
// The algorithms here are NORMATIVE per twanalyzers.h / twaspects.h — changing
// a step bumps the aspect version. All floating-point accumulation is done in
// double for stability; the emitted results are the documented float/uint types.

namespace {

// pi as a literal, not M_PI — MinGW does not define M_PI without _USE_MATH_DEFINES
// and we want no reliance on it.
constexpr double kPi = 3.14159265358979323846;

// --- little-endian scalar encoders -----------------------------------------
// Field-by-field, never a struct memcpy, so the params blob is byte-stable
// across compilers/padding (matches twqaf.cc house style).

inline void putU32( std::vector<uint8_t> &out, uint32_t v )
{
    out.push_back( (uint8_t)( v & 0xff ) );
    out.push_back( (uint8_t)( ( v >> 8 ) & 0xff ) );
    out.push_back( (uint8_t)( ( v >> 16 ) & 0xff ) );
    out.push_back( (uint8_t)( ( v >> 24 ) & 0xff ) );
}

inline void putF32( std::vector<uint8_t> &out, float v )
{
    uint32_t bits;
    std::memcpy( &bits, &v, 4 );   // bit-preserving float -> uint32
    putU32( out, bits );
}

// --- in-place radix-2 iterative complex FFT (double precision) --------------
// n must be a power of two. Decimation-in-time, Cooley-Tukey, bit-reversal
// permutation first. Twiddle factors are advanced by a fixed complex recurrence
// per stage — the same operations in the same order on every call, so the
// result is bit-reproducible on a given build.
void fftRadix2( std::vector<double> &re, std::vector<double> &im )
{
    const uint32_t n = (uint32_t)re.size();

    // Bit-reversal permutation.
    for( uint32_t i = 1, j = 0; i < n; i++ ) {
        uint32_t bit = n >> 1;
        for( ; j & bit; bit >>= 1 )
            j ^= bit;
        j ^= bit;
        if( i < j ) {
            std::swap( re[i], re[j] );
            std::swap( im[i], im[j] );
        }
    }

    for( uint32_t len = 2; len <= n; len <<= 1 ) {
        const double ang = -2.0 * kPi / (double)len;   // forward transform
        const double wRe = std::cos( ang );
        const double wIm = std::sin( ang );
        const uint32_t half = len >> 1;
        for( uint32_t base = 0; base < n; base += len ) {
            double curRe = 1.0, curIm = 0.0;
            for( uint32_t k = 0; k < half; k++ ) {
                const uint32_t a = base + k;
                const uint32_t b = a + half;
                const double vRe = re[b] * curRe - im[b] * curIm;
                const double vIm = re[b] * curIm + im[b] * curRe;
                re[b] = re[a] - vRe;
                im[b] = im[a] - vIm;
                re[a] = re[a] + vRe;
                im[a] = im[a] + vIm;
                const double nRe = curRe * wRe - curIm * wIm;
                const double nIm = curRe * wIm + curIm * wRe;
                curRe = nRe;
                curIm = nIm;
            }
        }
    }
}

// Hann-windowed magnitude spectrum. Reads up to `avail` real samples from src
// (the rest of the fftSize window is zero — the caller zero-pads past the end),
// applies w[j] = 0.5 - 0.5*cos(2*pi*j/(n-1)), transforms, and returns the
// magnitudes for bins [0, n/2] inclusive (n/2 + 1 values). n is a validated
// power of two.
std::vector<float> windowedMagnitudes( const float *src, uint32_t avail,
                                       uint32_t n )
{
    std::vector<double> re( n, 0.0 ), im( n, 0.0 );
    const double denom = (double)( n - 1 );
    for( uint32_t j = 0; j < n; j++ ) {
        const double w = 0.5 - 0.5 * std::cos( 2.0 * kPi * (double)j / denom );
        const double s = ( j < avail ) ? (double)src[j] : 0.0;
        re[j] = w * s;
    }
    fftRadix2( re, im );

    std::vector<float> mag( n / 2 + 1 );
    for( uint32_t b = 0; b <= n / 2; b++ )
        mag[b] = (float)std::sqrt( re[b] * re[b] + im[b] * im[b] );
    return mag;
}

} // namespace

// ---------------------------------------------------------------------------
// Test-only oracle hook. NOT declared in the public header on purpose: the
// analyzers_test re-declares it `extern` and compares this FFT path against an
// independent naive O(n^2) DFT. It Hann-windows the full n-sample input exactly
// as the analyzer does, so the test exercises the transform, not the framing.
// External linkage is deliberate; production code must not call it.
// ---------------------------------------------------------------------------
std::vector<float> twAnalyzerFftMagnitudes( const float *x, uint32_t n )
{
    if( n < 2 || ( n & ( n - 1 ) ) != 0 )
        return {};
    return windowedMagnitudes( x, n, n );
}

// ---------------------------------------------------------------------------
// twOnsetParams::serialize — canonical LE params blob (twaspects.h v1 order):
//   uint32 fftSize, uint32 hop, float32 thresholdFactor, float32 thresholdFloor,
//   uint32 medianHalfWidth, uint32 minSeparationFrames.
// ---------------------------------------------------------------------------
void twOnsetParams::serialize( std::vector<uint8_t> &out ) const
{
    out.clear();
    putU32( out, fftSize );
    putU32( out, hop );
    putF32( out, thresholdFactor );
    putF32( out, thresholdFloor );
    putU32( out, medianHalfWidth );
    putU32( out, minSeparationFrames );
}

// ---------------------------------------------------------------------------
// twLoudnessParams::serialize — LE params blob (twaspects.h v1 order):
//   uint32 hopFrames, uint32 winFrames.
// ---------------------------------------------------------------------------
void twLoudnessParams::serialize( std::vector<uint8_t> &out ) const
{
    out.clear();
    putU32( out, hopFrames );
    putU32( out, winFrames );
}

// ---------------------------------------------------------------------------
// twDetectOnsets — spectral-flux onset detection (aspect "onsets" v1). The
// steps below follow twanalyzers.h exactly.
// ---------------------------------------------------------------------------
std::vector<twOnset> twDetectOnsets( const float *const *chans, uint32_t nCh,
                                      uint64_t nFrames,
                                      const twOnsetParams &params )
{
    const uint32_t n   = params.fftSize;
    const uint32_t hop = params.hop;

    // Validation: fftSize a power of two >= 64; hop in (0, fftSize].
    if( n < 64 || ( n & ( n - 1 ) ) != 0 )
        return {};
    if( hop == 0 || hop > n )
        return {};
    if( chans == nullptr || nCh == 0 || nFrames == 0 )
        return {};

    // 1. mono[i] = arithmetic mean across channels.
    std::vector<float> mono( (size_t)nFrames );
    for( uint64_t i = 0; i < nFrames; i++ ) {
        double acc = 0.0;
        for( uint32_t c = 0; c < nCh; c++ )
            acc += (double)chans[c][i];
        mono[(size_t)i] = (float)( acc / (double)nCh );
    }

    // Frame count K: every k with k*hop < nFrames.
    const uint64_t K = ( nFrames - 1 ) / hop + 1;

    // 2 + 3. STFT magnitudes per frame, spectral flux against the predecessor.
    std::vector<double> flux( (size_t)K, 0.0 );
    std::vector<double> frameTot( (size_t)K, 0.0 );
    std::vector<float>  prevMag, curMag;
    for( uint64_t k = 0; k < K; k++ ) {
        const uint64_t t   = k * hop;                 // guaranteed < nFrames
        const uint64_t rem = nFrames - t;
        const uint32_t avail = ( rem >= n ) ? n : (uint32_t)rem;

        curMag = windowedMagnitudes( mono.data() + (size_t)t, avail, n );

        if( k == 0 ) {
            flux[0] = 0.0;                            // flux[0] = 0 by definition
        } else {
            double f = 0.0;
            double tot = 0.0;
            for( size_t b = 0; b < curMag.size(); b++ ) {
                const double d = (double)curMag[b] - (double)prevMag[b];
                if( d > 0.0 )
                    f += d;                           // positive differences only
                tot += (double)curMag[b];
            }
            // v2: NORMALIZED flux — RELATIVE spectral change. v1 summed raw
            // magnitude differences, so on steady loud material the
            // quantization-noise flux cleared the absolute floor and the
            // detector fired ~29 times/second on a pure sine — which the M4
            // onset keyframes then turned into level-collapsing phase resets.
            // Normalizing by the frame's total magnitude makes "no change"
            // read as ~0 regardless of level; a true attack reads O(1).
            flux[(size_t)k] = ( tot > 1e-12 ) ? f / tot : 0.0;
        }
        frameTot[(size_t)k] = 0.0;
        for( size_t b = 0; b < curMag.size(); b++ )
            frameTot[(size_t)k] += (double)curMag[b];
        prevMag.swap( curMag );
    }

    // v2 energy gate: a frame may host an onset only if it carries at least
    // 1% of the file's PEAK frame magnitude. Normalized flux alone explodes
    // on quiet crescendos (a near-silent signal doubling per hop is a huge
    // RELATIVE change but no perceptual onset); the gate confines detection
    // to audibly-present material. Loud-steady stays suppressed by the
    // normalization; true attacks pass both tests.
    double peakTot = 0.0;
    for( uint64_t k = 0; k < K; k++ )
        if( frameTot[(size_t)k] > peakTot ) peakTot = frameTot[(size_t)k];
    const double energyGate = 0.01 * peakTot;

    // 4. Median-adaptive threshold over the clamped window [k-W, k+W].
    const uint32_t      W = params.medianHalfWidth;
    std::vector<double> thr( (size_t)K, 0.0 );
    std::vector<double> scratch;
    for( uint64_t k = 0; k < K; k++ ) {
        const uint64_t lo = ( k >= W ) ? ( k - W ) : 0;
        uint64_t       hi = k + (uint64_t)W;
        if( hi >= K )
            hi = K - 1;

        scratch.clear();
        for( uint64_t i = lo; i <= hi; i++ )
            scratch.push_back( flux[(size_t)i] );

        // Median: partition into scratch, take the UPPER median — the element
        // at index size/2 after nth_element. For an odd count this is the true
        // middle; for an even count it is the higher of the two central values.
        // This tie/even-count rule is part of the determinism contract.
        const size_t mid = scratch.size() / 2;
        std::nth_element( scratch.begin(), scratch.begin() + mid, scratch.end() );
        const double med = scratch[mid];

        thr[(size_t)k] = (double)params.thresholdFactor * med
                       + (double)params.thresholdFloor;
    }

    // 5 + 6. Local-maximum peak picking, then ascending minimum-separation scan.
    // First and last frame are never candidates (they lack a neighbour).
    // v3: every detection carries its normalized flux as SALIENCE.
    // v4: the reported position is ATTACK-CENTERED. Flux at frame k measures
    // the transient ENTERING window [k*hop, k*hop+n) from its tail, so the
    // frame start k*hop leads the perceptual attack by ~(n - hop/2) — measured
    // -896 +/- 64 on the click corpus at the 1024/256 defaults, a visible
    // ~19 ms bias between UI ticks and the drawn waveform. Adding n - hop/2
    // centers the estimate on the attack (residual is hop quantization).
    const uint64_t attackLead = (uint64_t) n - hop / 2;
    std::vector<twOnset> out;
    bool     haveLast = false;
    uint64_t lastPos  = 0;
    for( uint64_t k = 1; k + 1 < K; k++ ) {
        const double f = flux[(size_t)k];
        if( f > thr[(size_t)k]
            && frameTot[(size_t)k] >= energyGate
            && f >= flux[(size_t)( k - 1 )]
            && f >  flux[(size_t)( k + 1 )] ) {
            uint64_t pos = k * hop + attackLead;
            if( pos >= nFrames ) pos = nFrames - 1;
            if( !haveLast
                || ( pos - lastPos ) >= (uint64_t)params.minSeparationFrames ) {
                out.push_back( { pos, (float) f } );
                lastPos  = pos;
                haveLast = true;
            }
        }
    }

    return out;
}

// ---------------------------------------------------------------------------
// twComputeLoudness — RMS envelope (aspect "loudness" v1).
//   rms[k] = sqrt( sum_{c,j} x[c][k*hop+j]^2 / (winFrames*nCh) ),
//   j in [0, winFrames), zero-padded past the end (the divisor stays
//   winFrames*nCh so tail records taper). Result size = ceil(nFrames/hopFrames).
// ---------------------------------------------------------------------------
std::vector<float> twComputeLoudness( const float *const *chans, uint32_t nCh,
                                      uint64_t nFrames,
                                      const twLoudnessParams &params )
{
    const uint32_t hop = params.hopFrames;
    const uint32_t win = params.winFrames;

    if( hop == 0 || win == 0 )
        return {};
    if( chans == nullptr || nCh == 0 || nFrames == 0 )
        return {};

    const uint64_t count = ( nFrames + hop - 1 ) / hop;   // ceil(nFrames/hop)
    const double   divisor = (double)win * (double)nCh;

    std::vector<float> out( (size_t)count );
    for( uint64_t k = 0; k < count; k++ ) {
        const uint64_t start = k * hop;
        double         acc   = 0.0;
        for( uint32_t j = 0; j < win; j++ ) {
            const uint64_t idx = start + j;
            if( idx >= nFrames )
                break;                                    // remainder is zero
            for( uint32_t c = 0; c < nCh; c++ ) {
                const double s = (double)chans[c][(size_t)idx];
                acc += s * s;
            }
        }
        out[(size_t)k] = (float)std::sqrt( acc / divisor );
    }

    return out;
}
