// analyzers_test.cc — unit tests for the offline analyzers (proposal 27 M1).
//
// House pattern (see sidecar_test.cc / io_vector_test.cc): plain main(), no
// framework, a CHECK(cond, msg) helper that records every failure so all
// sections run; main returns the failure count (0 == success). std::cout/cerr
// are allowed here — the tests/ directory is exempt from check_logging.
//
// Deterministic: an in-tree LCG (never rand()), fixed signals, exact byte
// expectations. Sections:
//   a. FFT oracle vs. an independent naive O(n^2) DFT.
//   b. Onset detection on a click train (+ silence, DC).
//   c. Loudness RMS envelope (constant, two-level, stereo fold).
//   d. Determinism (run twice, compare).
//   e. serialize() exact LE byte sequences.

#include "tw/sidecar/twanalyzers.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

// The analyzers' internal FFT, exposed for the oracle test only (see the note
// in twanalyzers.cc). NOT part of the public header.
extern std::vector<float> twAnalyzerFftMagnitudes( const float *x, uint32_t n );

// ---------------------------------------------------------------------------
// CHECK helper
// ---------------------------------------------------------------------------
static int g_fails = 0;
#define CHECK( cond, msg )                                                     \
    do {                                                                       \
        if ( !( cond ) ) {                                                     \
            std::cerr << "FAIL: " << ( msg ) << "  [" << __FILE__ << ":"       \
                      << __LINE__ << "]\n";                                    \
            ++g_fails;                                                         \
        }                                                                     \
    } while ( 0 )

static const double kPi = 3.14159265358979323846;

// Numerical-Recipes LCG in [0,1): fixed seed -> fixed sequence, never rand().
struct Lcg {
    uint32_t x;
    explicit Lcg( uint32_t seed ) : x( seed ) {}
    float next() {
        x = x * 1664525u + 1013904223u;
        // Map the high 24 bits to [-1, 1).
        return ( (float)( x >> 8 ) / (float)( 1u << 24 ) ) * 2.0f - 1.0f;
    }
};

// ===========================================================================
// Section a — FFT oracle: our FFT path vs. an independent naive DFT.
// ===========================================================================

// Naive O(n^2) DFT of a Hann-windowed real input, returning magnitudes for
// bins [0, n/2]. Windowing matches windowedMagnitudes() in twanalyzers.cc so
// the comparison isolates the transform.
static std::vector<float> naiveDftMagnitudes( const std::vector<float> &x ) {
    const uint32_t n = (uint32_t)x.size();
    std::vector<double> win( n );
    for ( uint32_t j = 0; j < n; ++j ) {
        double w = 0.5 - 0.5 * std::cos( 2.0 * kPi * (double)j / (double)( n - 1 ) );
        win[j]   = w * (double)x[j];
    }
    std::vector<float> mag( n / 2 + 1 );
    for ( uint32_t b = 0; b <= n / 2; ++b ) {
        double re = 0.0, im = 0.0;
        for ( uint32_t j = 0; j < n; ++j ) {
            double ang = -2.0 * kPi * (double)b * (double)j / (double)n;
            re += win[j] * std::cos( ang );
            im += win[j] * std::sin( ang );
        }
        mag[b] = (float)std::sqrt( re * re + im * im );
    }
    return mag;
}

static void section_a_fft() {
    std::cout << "== Section a: FFT oracle ==\n";
    const uint32_t sizes[] = { 64, 256 };
    for ( uint32_t n : sizes ) {
        Lcg                lcg( 0xA1B2C3D4u ^ n );
        std::vector<float> x( n );
        for ( uint32_t j = 0; j < n; ++j )
            x[j] = lcg.next();

        std::vector<float> fftMag = twAnalyzerFftMagnitudes( x.data(), n );
        std::vector<float> refMag = naiveDftMagnitudes( x );

        CHECK( fftMag.size() == n / 2 + 1, "FFT returns n/2+1 bins" );
        CHECK( refMag.size() == fftMag.size(), "DFT/FFT bin count agree" );

        double worst = 0.0;
        for ( uint32_t b = 0; b < fftMag.size(); ++b ) {
            double ref = refMag[b];
            if ( ref <= 1e-6 )
                continue;   // skip near-zero bins per spec
            double rel = std::fabs( (double)fftMag[b] - ref ) / ref;
            if ( rel > worst )
                worst = rel;
        }
        std::cout << "  n=" << n << " worst relative error " << worst << "\n";
        CHECK( worst < 1e-4, "FFT matches naive DFT (rel err < 1e-4)" );
    }
}

// ===========================================================================
// Section b — onset detection.
// ===========================================================================
static void section_b_onsets() {
    std::cout << "== Section b: onsets ==\n";

    const uint64_t nFrames = 480000;    // 10 s @ 48 kHz
    const uint64_t clicks[] = { 48000, 120000, 240000, 360000, 432000 };
    const int      nClicks   = 5;

    // Click train: unit impulses in silence.
    std::vector<float> sig( (size_t)nFrames, 0.0f );
    for ( int i = 0; i < nClicks; ++i )
        sig[(size_t)clicks[i]] = 1.0f;
    const float *chans[1] = { sig.data() };

    twOnsetParams p;                    // defaults (fftSize 1024, hop 256, ...)
    p.minSeparationFrames = 1440;

    std::vector<twOnset> onsets = twDetectOnsets( chans, 1, nFrames, p );

    CHECK( onsets.size() == 5, "click train yields exactly 5 onsets" );

    // 1:1 pairing: each click matched by exactly one onset within
    // +/-fftSize: a click influences every frame whose window contains it,
    // so a detection may sit up to one window span from the click. v2's
    // normalized flux favors the EARLIEST such frame (smallest total
    // magnitude), typically ~fftSize-hop early — inside this bound.
    const uint64_t tol = p.fftSize;               // 1024
    std::vector<bool> matched( onsets.size(), false );
    for ( int i = 0; i < nClicks; ++i ) {
        int hit = -1;
        for ( size_t j = 0; j < onsets.size(); ++j ) {
            if ( matched[j] )
                continue;
            uint64_t a = onsets[j].pos, b = clicks[i];
            uint64_t d = ( a > b ) ? ( a - b ) : ( b - a );
            if ( d <= tol ) {
                hit = (int)j;
                break;
            }
        }
        CHECK( hit >= 0, "each click matched by an onset within +/-768" );
        if ( hit >= 0 ) {
            matched[hit] = true;
            long long off = (long long)onsets[hit].pos - (long long)clicks[i];
            std::cout << "  click " << clicks[i] << " -> onset "
                      << onsets[hit].pos << " (salience "
                      << onsets[hit].salience << ", offset " << off << ")\n";
        }
    }

    // Pure silence -> 0 onsets.
    {
        std::vector<float> z( 96000, 0.0f );
        const float       *zc[1] = { z.data() };
        twOnsetParams      sp;
        std::vector<twOnset> r = twDetectOnsets( zc, 1, z.size(), sp );
        CHECK( r.empty(), "pure silence -> 0 onsets" );
    }

    // DC signal (constant 0.5): no transient anywhere in its interior. The one
    // unavoidable artifact is the buffer's own hard end — zero-padding a
    // non-silent tail frame spreads a truncated Hann window across all bins,
    // which spectral flux legitimately reads as a positive edge. (Silence and
    // the click train both END in zeros, so they have no such edge — that is
    // precisely why the click train yields exactly its 5.) The meaningful DC
    // property is therefore: ZERO onsets in the steady interior; any detection
    // is confined to the final fftSize-frame truncation boundary.
    {
        std::vector<float> dc( 96000, 0.5f );
        const float       *dcc[1] = { dc.data() };
        twOnsetParams      dp;
        std::vector<twOnset> r = twDetectOnsets( dcc, 1, dc.size(), dp );
        const uint64_t interiorEnd = dc.size() - dp.fftSize;   // last full frame start
        int inInterior = 0;
        for ( const twOnset &o : r )
            if ( o.pos < interiorEnd )
                ++inInterior;
        std::cout << "  DC: " << r.size() << " onset(s), " << inInterior
                  << " in interior (expect 0; boundary edge at frames >= "
                  << interiorEnd << ")\n";
        CHECK( inInterior == 0,
               "DC signal -> 0 interior onsets (only the buffer-end edge, if any)" );
    }

    // Invalid params -> empty.
    {
        twOnsetParams bad;
        bad.fftSize = 1000;             // not a power of two
        CHECK( twDetectOnsets( chans, 1, nFrames, bad ).empty(),
               "non-power-of-two fftSize -> empty" );
        twOnsetParams bad2;
        bad2.fftSize = 32;              // < 64
        CHECK( twDetectOnsets( chans, 1, nFrames, bad2 ).empty(),
               "fftSize < 64 -> empty" );
        twOnsetParams bad3;
        bad3.hop = 0;
        CHECK( twDetectOnsets( chans, 1, nFrames, bad3 ).empty(),
               "hop == 0 -> empty" );
        twOnsetParams bad4;
        bad4.hop = bad4.fftSize + 1;    // hop > fftSize
        CHECK( twDetectOnsets( chans, 1, nFrames, bad4 ).empty(),
               "hop > fftSize -> empty" );
    }
}

// ===========================================================================
// Section c — loudness RMS envelope.
// ===========================================================================
static void section_c_loudness() {
    std::cout << "== Section c: loudness ==\n";

    const uint32_t rate = 48000;
    const uint64_t n    = 48000;        // 1 s
    const double   root2 = std::sqrt( 2.0 );

    // Sine helper.
    auto makeSine = []( uint64_t len, double amp, double freq, uint32_t sr ) {
        std::vector<float> v( (size_t)len );
        for ( uint64_t i = 0; i < len; ++i )
            v[(size_t)i] =
                (float)( amp * std::sin( 2.0 * kPi * freq * (double)i / (double)sr ) );
        return v;
    };

    twLoudnessParams lp;
    lp.hopFrames = 480;
    lp.winFrames = 960;

    // (1) Constant-amplitude sine A = 0.5, 440 Hz -> interior ~ A/sqrt(2).
    {
        std::vector<float> s = makeSine( n, 0.5, 440.0, rate );
        const float       *c[1] = { s.data() };
        std::vector<float> rms = twComputeLoudness( c, 1, n, lp );
        CHECK( !rms.empty(), "loudness non-empty for valid input" );
        CHECK( rms.size() == ( n + lp.hopFrames - 1 ) / lp.hopFrames,
               "loudness record count == ceil(n/hop)" );
        const double expect = 0.5 / root2;   // ~0.35355
        double       worst  = 0.0;
        for ( size_t k = 2; k + 4 <= rms.size(); ++k ) {
            double rel = std::fabs( (double)rms[k] - expect ) / expect;
            if ( rel > worst )
                worst = rel;
        }
        std::cout << "  constant sine: worst interior rel err " << worst << "\n";
        CHECK( worst < 0.02, "constant-sine interior RMS within 2% of A/sqrt2" );
    }

    // (2) Two-level: first half amp 0.25, second half amp 0.5.
    {
        std::vector<float> a = makeSine( n / 2, 0.25, 440.0, rate );
        std::vector<float> b = makeSine( n / 2, 0.50, 440.0, rate );
        std::vector<float> s;
        s.reserve( (size_t)n );
        s.insert( s.end(), a.begin(), a.end() );
        s.insert( s.end(), b.begin(), b.end() );
        const float       *c[1] = { s.data() };
        std::vector<float> rms   = twComputeLoudness( c, 1, n, lp );

        const uint64_t halfRec = ( n / 2 ) / lp.hopFrames;   // records in half 1
        const double   e1      = 0.25 / root2;
        const double   e2      = 0.50 / root2;
        double         w1 = 0.0, w2 = 0.0;
        // Interior of first half: [2, halfRec-4].
        for ( uint64_t k = 2; k + 4 <= halfRec; ++k ) {
            double rel = std::fabs( (double)rms[(size_t)k] - e1 ) / e1;
            if ( rel > w1 )
                w1 = rel;
        }
        // Interior of second half: [halfRec+2, count-4].
        for ( uint64_t k = halfRec + 2; k + 4 <= rms.size(); ++k ) {
            double rel = std::fabs( (double)rms[(size_t)k] - e2 ) / e2;
            if ( rel > w2 )
                w2 = rel;
        }
        std::cout << "  two-level: worst rel err half1 " << w1
                  << " half2 " << w2 << "\n";
        CHECK( w1 < 0.02, "two-level first-half interior within 2% of 0.1768" );
        CHECK( w2 < 0.02, "two-level second-half interior within 2% of 0.3536" );
    }

    // (3) Stereo fold: sines amp 0.3 and 0.4 -> power-mean RMS.
    {
        std::vector<float> l = makeSine( n, 0.3, 440.0, rate );
        std::vector<float> r = makeSine( n, 0.4, 440.0, rate );
        const float       *c[2] = { l.data(), r.data() };
        std::vector<float> rms   = twComputeLoudness( c, 2, n, lp );
        const double expect =
            std::sqrt( ( 0.3 * 0.3 + 0.4 * 0.4 ) / 2.0 ) / root2;   // 0.25
        double worst = 0.0;
        for ( size_t k = 2; k + 4 <= rms.size(); ++k ) {
            double rel = std::fabs( (double)rms[k] - expect ) / expect;
            if ( rel > worst )
                worst = rel;
        }
        std::cout << "  stereo fold: expect " << expect
                  << " worst interior rel err " << worst << "\n";
        CHECK( worst < 0.02, "stereo-fold interior RMS within 2% of 0.25" );
    }

    // (4) Empty / invalid params -> empty result.
    {
        std::vector<float> s = makeSine( n, 0.5, 440.0, rate );
        const float       *c[1] = { s.data() };
        twLoudnessParams   bad;         // hop==0, win==0 by default
        CHECK( twComputeLoudness( c, 1, n, bad ).empty(),
               "default (zero) loudness params -> empty" );
        twLoudnessParams   bad2;
        bad2.hopFrames = 480;
        bad2.winFrames = 0;
        CHECK( twComputeLoudness( c, 1, n, bad2 ).empty(),
               "winFrames == 0 -> empty" );
        twLoudnessParams   bad3;
        bad3.hopFrames = 0;
        bad3.winFrames = 960;
        CHECK( twComputeLoudness( c, 1, n, bad3 ).empty(),
               "hopFrames == 0 -> empty" );
    }
}

// ===========================================================================
// Section d — determinism: run twice, compare bit-for-bit.
// ===========================================================================
static void section_d_determinism() {
    std::cout << "== Section d: determinism ==\n";

    const uint64_t nFrames = 480000;
    const uint64_t clicks[] = { 48000, 120000, 240000, 360000, 432000 };
    std::vector<float> sig( (size_t)nFrames, 0.0f );
    for ( uint64_t c : clicks )
        sig[(size_t)c] = 1.0f;
    const float *chans[1] = { sig.data() };

    twOnsetParams op;
    op.minSeparationFrames = 1440;
    std::vector<twOnset> o1 = twDetectOnsets( chans, 1, nFrames, op );
    std::vector<twOnset> o2 = twDetectOnsets( chans, 1, nFrames, op );
    CHECK( o1 == o2, "twDetectOnsets deterministic (identical twice)" );

    std::vector<float> s( (size_t)48000 );
    for ( size_t i = 0; i < s.size(); ++i )
        s[i] = (float)( 0.5 * std::sin( 2.0 * kPi * 440.0 * (double)i / 48000.0 ) );
    const float      *sc[1] = { s.data() };
    twLoudnessParams  lp;
    lp.hopFrames = 480;
    lp.winFrames = 960;
    std::vector<float> r1 = twComputeLoudness( sc, 1, s.size(), lp );
    std::vector<float> r2 = twComputeLoudness( sc, 1, s.size(), lp );
    CHECK( r1 == r2, "twComputeLoudness deterministic (identical twice)" );
}

// ===========================================================================
// Section e — serialize(): exact little-endian byte sequences.
// ===========================================================================
static void appendU32LE( std::vector<uint8_t> &v, uint32_t x ) {
    v.push_back( (uint8_t)( x & 0xff ) );
    v.push_back( (uint8_t)( ( x >> 8 ) & 0xff ) );
    v.push_back( (uint8_t)( ( x >> 16 ) & 0xff ) );
    v.push_back( (uint8_t)( ( x >> 24 ) & 0xff ) );
}
static void appendF32LE( std::vector<uint8_t> &v, float f ) {
    uint32_t bits;
    std::memcpy( &bits, &f, 4 );
    appendU32LE( v, bits );
}

static void section_e_serialize() {
    std::cout << "== Section e: serialize ==\n";

    // Onset params: known values, exact 24-byte LE blob.
    {
        twOnsetParams p;
        p.fftSize             = 1024;
        p.hop                 = 256;
        p.thresholdFactor     = 1.5f;
        p.thresholdFloor      = 1.0e-4f;
        p.medianHalfWidth     = 8;
        p.minSeparationFrames = 1440;

        std::vector<uint8_t> got;
        p.serialize( got );

        std::vector<uint8_t> want;
        appendU32LE( want, 1024 );
        appendU32LE( want, 256 );
        appendF32LE( want, 1.5f );
        appendF32LE( want, 1.0e-4f );
        appendU32LE( want, 8 );
        appendU32LE( want, 1440 );

        CHECK( got.size() == 24, "onset params blob is 24 bytes" );
        CHECK( got == want, "onset params serialize == expected LE bytes" );
    }

    // Loudness params: known values, exact 8-byte LE blob.
    {
        twLoudnessParams p;
        p.hopFrames = 480;
        p.winFrames = 960;

        std::vector<uint8_t> got;
        p.serialize( got );

        std::vector<uint8_t> want;
        appendU32LE( want, 480 );
        appendU32LE( want, 960 );

        CHECK( got.size() == 8, "loudness params blob is 8 bytes" );
        CHECK( got == want, "loudness params serialize == expected LE bytes" );
    }
}

// ===========================================================================
// main
// ===========================================================================
int main() {
    std::cout << "analyzers_test starting\n";

    section_a_fft();
    section_b_onsets();
    section_c_loudness();
    section_d_determinism();
    section_e_serialize();

    if ( g_fails == 0 )
        std::cout << "\nAll analyzers tests passed.\n";
    else
        std::cout << "\n" << g_fails << " analyzers test check(s) FAILED.\n";

    return g_fails;
}
