// warp_bench.cc — DSP hot-path benchmark (proposal 28 W5 SIMD tripwire).
//
// NOT a gate: this binary produces TIMINGS, not pass/fail. It exists so the
// W5 work is measurement-driven — run before and after each optimization and
// keep the numbers in the commit message. Fixed seeds and fixed signals, so
// successive runs on one machine are comparable; wall-clock via
// steady_clock (tests are exempt from the no-time rule that scripts have).
//
// Scenarios (chosen from the real workloads):
//   1. vocoder stretch      — playback/readahead-shaped: warpOffline 1.2x
//   2. vocoder stretch+pitch— the full pipeline incl. sinc resample
//   3. vocoder formants     — W4 path (envelope FFT pair per frame)
//   4. paged window         — one 65536-frame page render mid-signal (the
//                             drag-feedback latency proxy: one page is what
//                             the scheduler asks for after an edit)
//   5. f0 analysis          — twComputeF0 (the import-lane heavyweight)
//   6. onsets analysis      — twDetectOnsets (import lane)

#include "tw/sources/twpagedvocoder.h"
#include "tw/sidecar/twanalyzers.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

static double now_ms()
{
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch() ).count();
}

int main()
{
    const uint32_t rate  = 48000;
    const uint64_t inLen = 30 * rate;             // 30 s stereo
    std::vector<float> L( (size_t) inLen ), R( (size_t) inLen );
    uint32_t lcg = 0xBEEFCAFEu;
    for( uint64_t i = 0; i < inLen; i++ ) {
        lcg = lcg * 1664525u + 1013904223u;
        const double noise = ( (double) ( lcg >> 16 ) / 32768.0 - 1.0 ) * 0.05;
        const double s = 0.4 * std::sin( 2.0 * 3.14159265358979323846
                                         * 220.0 * (double) i / rate );
        const double s2 = 0.2 * std::sin( 2.0 * 3.14159265358979323846
                                          * 913.0 * (double) i / rate );
        L[(size_t) i] = (float) ( s + s2 + noise );
        R[(size_t) i] = (float) ( 0.8 * s - s2 + noise );
    }
    const float *chans[2] = { L.data(), R.data() };

    std::vector<uint64_t> onsets;
    for( uint64_t p = 24000; p < inLen; p += 48000 ) onsets.push_back( p );

    twPagedVocoder::Config cfg;
    cfg.rate     = (int) rate;
    cfg.channels = 2;

    struct Row { const char *name; double ms; double xrt; };
    std::vector<Row> rows;
    const double inSec = (double) inLen / rate;

    // 1. stretch only
    {
        const double st = 1.2;
        const uint64_t outLen = (uint64_t) std::floor( (double) inLen * st );
        std::vector<float> out( (size_t) 2 * outLen, 0.0f );
        const double t0 = now_ms();
        twPagedVocoder::warpOffline( chans, inLen, out.data(), outLen, cfg,
                                     st, 1.0, onsets.data(), onsets.size() );
        const double dt = now_ms() - t0;
        rows.push_back( { "stretch 1.2x        ", dt, inSec * 1000.0 / dt } );
    }

    // 2. stretch + pitch
    {
        const double st = 1.2, pr = 1.259921;
        const uint64_t outLen = (uint64_t) std::floor( (double) inLen * st );
        std::vector<float> out( (size_t) 2 * outLen, 0.0f );
        const double t0 = now_ms();
        twPagedVocoder::warpOffline( chans, inLen, out.data(), outLen, cfg,
                                     st, pr, onsets.data(), onsets.size() );
        const double dt = now_ms() - t0;
        rows.push_back( { "stretch+pitch       ", dt, inSec * 1000.0 / dt } );
    }

    // 3. stretch + pitch + formants (W4)
    {
        twPagedVocoder::Config fc = cfg;
        fc.preserveFormants = true;
        const double st = 1.2, pr = 1.259921;
        const uint64_t outLen = (uint64_t) std::floor( (double) inLen * st );
        std::vector<float> out( (size_t) 2 * outLen, 0.0f );
        const double t0 = now_ms();
        twPagedVocoder::warpOffline( chans, inLen, out.data(), outLen, fc,
                                     st, pr, onsets.data(), onsets.size() );
        const double dt = now_ms() - t0;
        rows.push_back( { "stretch+pitch+fmt   ", dt, inSec * 1000.0 / dt } );
    }

    // 4. one page mid-signal, fresh instance (drag-feedback latency proxy)
    {
        const double st = 1.2, pr = 1.259921;
        const uint64_t outLen = (uint64_t) std::floor( (double) inLen * st );
        const uint64_t page = 65536;
        const uint64_t at   = ( outLen / 2 ) & ~( page - 1 );
        std::vector<float> out( (size_t) 2 * page, 0.0f );
        const double t0 = now_ms();
        twPagedVocoder v( chans, inLen, cfg, st, pr,
                          onsets.data(), onsets.size() );
        v.render( at, page, out.data() );
        const double dt = now_ms() - t0;
        rows.push_back( { "one page (65536)    ", dt,
                          ( page / (double) rate ) * 1000.0 / dt } );
    }

    // 5. f0 (import lane)
    {
        twF0Params p;
        p.rate      = rate;
        p.hopFrames = rate / 100;
        p.winFrames = rate / 30;
        const double t0 = now_ms();
        std::vector<float> f0 = twComputeF0( chans, 2, inLen, p );
        const double dt = now_ms() - t0;
        rows.push_back( { "f0 (YIN)            ", dt, inSec * 1000.0 / dt } );
        if( f0.empty() ) std::cout << "(f0 empty?!)\n";
    }

    // 6. onsets (import lane)
    {
        twOnsetParams p;
        p.minSeparationFrames = rate * 3 / 100;
        const double t0 = now_ms();
        std::vector<twOnset> o = twDetectOnsets( chans, 2, inLen, p );
        const double dt = now_ms() - t0;
        rows.push_back( { "onsets (flux)       ", dt, inSec * 1000.0 / dt } );
        std::cout << "(onsets: " << o.size() << ")\n";
    }

    std::cout << "\nwarp_bench — 30 s stereo @ 48 kHz\n";
    std::cout << "  scenario                  ms      x-realtime\n";
    for( const Row &r : rows )
        std::cout << "  " << r.name << "  " << (int) r.ms
                  << "\t" << r.xrt << "\n";
    return 0;
}
