// vocoder_test.cc — twPagedVocoder M4 property gates (proposal 27).
//
// The load-bearing property: PAGED OUTPUT IS BIT-IDENTICAL TO WHOLE-SIGNAL
// OUTPUT for any partition of the output range, including when every window
// is rendered by a FRESH instance (no hidden sequential state). This is what
// makes the vocoder safe under the page scheduler at any worker count.
//
// House pattern: plain main(), CHECK macro, returns failure count.

#include "tw/sources/twpagedvocoder.h"

#include <cmath>
#include <cstring>
#include <iostream>
#include <vector>

static int g_fails = 0;
#define CHECK( cond, msg )                                                    \
    do {                                                                      \
        if( !( cond ) ) {                                                     \
            std::cerr << "FAIL: " << ( msg ) << "  [" << __LINE__ << "]\n";   \
            ++g_fails;                                                        \
        }                                                                     \
    } while( 0 )

namespace {

// Deterministic test signal: sine + LCG noise floor + hard clicks (stereo).
void makeSignal( std::vector<float> &l, std::vector<float> &r, uint64_t n )
{
    l.resize( (size_t) n );
    r.resize( (size_t) n );
    uint32_t lcg = 0x5EED5EEDu;
    for( uint64_t i = 0; i < n; i++ ) {
        lcg = lcg * 1664525u + 1013904223u;
        const double noise = ( (double) ( lcg >> 16 ) / 32768.0 - 1.0 ) * 0.05;
        const double s = 0.4 * std::sin( 2.0 * 3.14159265358979323846
                                         * 440.0 * (double) i / 48000.0 );
        double click = 0.0;
        if( i % 24000 < 40 ) click = ( i % 2 ) ? -0.5 : 0.5;
        l[(size_t) i] = (float) ( s + noise + click );
        r[(size_t) i] = (float) ( 0.7 * s - noise + click );
    }
}

struct Case {
    double stretch, pitchRatio;
};

// LCG-driven partition of [0, total) into windows of 97..~40000 frames.
std::vector<uint64_t> makeCuts( uint64_t total, uint32_t seed )
{
    std::vector<uint64_t> cuts;
    cuts.push_back( 0 );
    uint32_t lcg = seed;
    uint64_t pos = 0;
    while( pos < total ) {
        lcg = lcg * 1664525u + 1013904223u;
        pos += 97 + ( lcg % 40000 );
        cuts.push_back( pos < total ? pos : total );
    }
    return cuts;
}

} // namespace

int main()
{
    std::cout << "vocoder_test starting\n";

    const uint64_t inLen = 3 * 48000;
    std::vector<float> L, R;
    makeSignal( L, R, inLen );
    const float *chans[2] = { L.data(), R.data() };

    const uint64_t onsets[] = { 24000, 48000, 96000, 120000 };
    const size_t   nOnsets  = 4;

    twPagedVocoder::Config cfg;
    cfg.rate     = 48000;
    cfg.channels = 2;

    const Case cases[] = {
        { 2.0, 1.0 },
        { 0.5, 1.0 },
        { 1.37, 1.259921 },   // stretch + pitch up
        { 1.0, 0.6300 },      // pitch-only down
    };

    for( const Case &tc : cases ) {
        const uint64_t outLen =
            (uint64_t) std::floor( (double) inLen * tc.stretch );

        // Reference: whole-signal render.
        std::vector<float> whole( (size_t) 2 * outLen, 0.0f );
        twPagedVocoder::warpOffline( chans, inLen, whole.data(), outLen, cfg,
                                     tc.stretch, tc.pitchRatio,
                                     onsets, nOnsets );

        // Determinism: whole-signal twice → identical bytes.
        {
            std::vector<float> again( (size_t) 2 * outLen, 0.0f );
            twPagedVocoder::warpOffline( chans, inLen, again.data(), outLen,
                                         cfg, tc.stretch, tc.pitchRatio,
                                         onsets, nOnsets );
            CHECK( std::memcmp( whole.data(), again.data(),
                                whole.size() * sizeof( float ) ) == 0,
                   "whole-signal render is deterministic" );
        }

        // Partition property, one persistent instance: windows in order.
        {
            twPagedVocoder v( chans, inLen, cfg, tc.stretch, tc.pitchRatio,
                              onsets, nOnsets );
            std::vector<float> paged( (size_t) 2 * outLen, 0.0f );
            const std::vector<uint64_t> cuts = makeCuts( outLen, 0xA5F00Du );
            for( size_t i = 0; i + 1 < cuts.size(); i++ ) {
                const uint64_t a = cuts[i], b = cuts[i + 1];
                const uint64_t len = b - a;
                std::vector<float> w( (size_t) 2 * len, 0.0f );
                v.render( a, len, w.data() );
                for( uint32_t c = 0; c < 2; c++ )
                    std::memcpy( paged.data() + (size_t) c * outLen + a,
                                 w.data() + (size_t) c * len,
                                 (size_t) len * sizeof( float ) );
            }
            CHECK( std::memcmp( whole.data(), paged.data(),
                                whole.size() * sizeof( float ) ) == 0,
                   "partitioned render (one instance) == whole render" );
        }

        // Partition property, FRESH instance per window and a DIFFERENT
        // partition — no sequential state may leak between windows.
        {
            std::vector<float> paged( (size_t) 2 * outLen, 0.0f );
            const std::vector<uint64_t> cuts = makeCuts( outLen, 0x0BADCAFE );
            for( size_t i = 0; i + 1 < cuts.size(); i++ ) {
                const uint64_t a = cuts[i], b = cuts[i + 1];
                const uint64_t len = b - a;
                twPagedVocoder v( chans, inLen, cfg, tc.stretch,
                                  tc.pitchRatio, onsets, nOnsets );
                std::vector<float> w( (size_t) 2 * len, 0.0f );
                v.render( a, len, w.data() );
                for( uint32_t c = 0; c < 2; c++ )
                    std::memcpy( paged.data() + (size_t) c * outLen + a,
                                 w.data() + (size_t) c * len,
                                 (size_t) len * sizeof( float ) );
            }
            CHECK( std::memcmp( whole.data(), paged.data(),
                                whole.size() * sizeof( float ) ) == 0,
                   "fresh-instance-per-window render == whole render" );
        }

        // Sanity: output is not silence (the warp actually produced signal).
        {
            double e = 0.0;
            for( uint64_t i = 0; i < outLen; i++ )
                e += (double) whole[(size_t) i] * whole[(size_t) i];
            CHECK( e > 1.0, "warped output carries energy" );
        }
    }

    // Onset-set sensitivity: different keyframes → (almost surely)
    // different bytes, proving onsets actually participate.
    {
        const uint64_t outLen = 2 * inLen;
        std::vector<float> a( (size_t) 2 * outLen, 0.0f );
        std::vector<float> b( (size_t) 2 * outLen, 0.0f );
        twPagedVocoder::warpOffline( chans, inLen, a.data(), outLen, cfg,
                                     2.0, 1.0, onsets, nOnsets );
        twPagedVocoder::warpOffline( chans, inLen, b.data(), outLen, cfg,
                                     2.0, 1.0, nullptr, 0 );
        CHECK( std::memcmp( a.data(), b.data(),
                            a.size() * sizeof( float ) ) != 0,
               "onset keyframes change the rendered bytes" );
    }

    if( g_fails == 0 )
        std::cout << "\nAll vocoder tests passed.\n";
    else
        std::cout << "\n" << g_fails << " vocoder test check(s) FAILED.\n";
    return g_fails;
}
