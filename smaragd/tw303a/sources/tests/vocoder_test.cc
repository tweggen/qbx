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

    // --- W1: USER WARP MAP — the partition property must hold under
    // piecewise maps too (fresh instance per window, random partition), and
    // user anchors must actually reshape time.
    {
        twPagedVocoder::Config mc = cfg;
        // Internal-domain breakpoints (out, in): first half stretched 2x,
        // then compressed to 0.75x, final slope extends.
        mc.userMap = { { 96000.0, 48000.0 }, { 132000.0, 96000.0 } };
        const uint64_t outLen = 96000 + 36000 + 36000;   // spans + extension
        std::vector<float> whole( (size_t) 2 * outLen, 0.0f );
        twPagedVocoder::warpOffline( chans, inLen, whole.data(), outLen, cfg,
                                     1.0, 1.0, onsets, nOnsets );
        // (baseline above is the identity map — must differ from mapped)
        std::vector<float> mapped( (size_t) 2 * outLen, 0.0f );
        {
            twPagedVocoder v( chans, inLen, mc, 1.0, 1.0, onsets, nOnsets );
            v.render( 0, outLen, mapped.data() );
        }
        CHECK( std::memcmp( whole.data(), mapped.data(),
                            whole.size() * sizeof( float ) ) != 0,
               "user map reshapes the output" );

        // Partition property under the user map, fresh instances.
        std::vector<float> paged( (size_t) 2 * outLen, 0.0f );
        const std::vector<uint64_t> cuts = makeCuts( outLen, 0xC0FFEE01 );
        for( size_t i = 0; i + 1 < cuts.size(); i++ ) {
            const uint64_t a = cuts[i], b = cuts[i + 1];
            const uint64_t len = b - a;
            twPagedVocoder v( chans, inLen, mc, 1.0, 1.0, onsets, nOnsets );
            std::vector<float> w( (size_t) 2 * len, 0.0f );
            v.render( a, len, w.data() );
            for( uint32_t c = 0; c < 2; c++ )
                std::memcpy( paged.data() + (size_t) c * outLen + a,
                             w.data() + (size_t) c * len,
                             (size_t) len * sizeof( float ) );
        }
        CHECK( std::memcmp( mapped.data(), paged.data(),
                            mapped.size() * sizeof( float ) ) == 0,
               "user-map paged (fresh instances) == whole render" );

        // Map + pitch compose (partition property again, coarser check).
        {
            const uint64_t pOut = outLen;
            std::vector<float> a1( (size_t) 2 * pOut, 0.0f );
            std::vector<float> a2( (size_t) 2 * pOut, 0.0f );
            twPagedVocoder v1( chans, inLen, mc, 1.0, 1.259921, onsets,
                               nOnsets );
            v1.render( 0, pOut, a1.data() );
            twPagedVocoder v2( chans, inLen, mc, 1.0, 1.259921, onsets,
                               nOnsets );
            const uint64_t half = pOut / 2;
            std::vector<float> w1( (size_t) 2 * half, 0.0f );
            std::vector<float> w2( (size_t) 2 * ( pOut - half ), 0.0f );
            v2.render( 0, half, w1.data() );
            v2.render( half, pOut - half, w2.data() );
            for( uint32_t c = 0; c < 2; c++ ) {
                std::memcpy( a2.data() + (size_t) c * pOut,
                             w1.data() + (size_t) c * half,
                             (size_t) half * sizeof( float ) );
                std::memcpy( a2.data() + (size_t) c * pOut + half,
                             w2.data() + (size_t) c * ( pOut - half ),
                             (size_t) ( pOut - half ) * sizeof( float ) );
            }
            CHECK( std::memcmp( a1.data(), a2.data(),
                                a1.size() * sizeof( float ) ) == 0,
                   "user map + pitch: split render == whole render" );
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
