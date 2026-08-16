#ifndef _TW_LEVEL_SCAN_H_
#define _TW_LEVEL_SCAN_H_

// Proposal 34 — the leaf of the metering module: one pass over a span of mono
// samples. Pure function, no state, no allocation, safe from any thread.

#include "tw/core/twtypes.h"

#include <cstdint>

// The default clip threshold. Not 1.0: a float sample that round-tripped
// through a 16-bit PCM stage lands on 32767/32768 = 0.99997, which IS a clip
// for every practical purpose.
#define TW_METER_CLIP_THRESHOLD 0.999f

// One measurement over a contiguous span of mono samples.
struct twLevelSample {
    float    peak       = 0.0f;   // max |s| over the span, linear, >= 0
    float    meanSquare = 0.0f;   // mean of s^2 over the span, linear, >= 0
    uint32_t frames     = 0;      // frames actually scanned (0 == no measurement)
    bool     clipped    = false;  // any |s| >= the clip threshold
};

// One measurement PER LANE, for a meter that shows a project's channels
// side by side (proposal 36 B8).
//
// twLevelSample stays scalar and twScanSpan stays a one-span function — a lane
// IS one span, and a set is N of them. That is deliberate: everything below the
// set (the scan, the ballistics) keeps working on exactly one lane, so the only
// things that had to learn about channels are the probe (which lane of which
// page) and the widget (how to draw N bars).
struct twLevelSampleSet {
    // The widest project the engine accepts (proposal 36 M1: 1/2/4/6/8).
    static constexpr int MAX_LANES = 8;

    twLevelSample lane[MAX_LANES];
    int           lanes = 0;      // 0 == no measurement at all

    // The lane a scalar caller means. Never out of range: an empty set answers
    // a default (frames == 0) sample, which IS "no measurement".
    const twLevelSample &first() const
    {
        static const twLevelSample none;
        return lanes > 0 ? lane[0] : none;
    }
};

// Scan [p, p+n) for peak, mean-square and clipping in a single pass.
// n <= 0 or p == nullptr yields an all-zero sample with frames == 0, which
// callers must treat as "no measurement", NOT as "silence".
inline twLevelSample twScanSpan( const sample_t *p, length_t n,
                                 float clipThreshold = TW_METER_CLIP_THRESHOLD )
{
    twLevelSample out;
    if( !p || n <= 0 ) return out;

    float  peak = 0.0f;
    double sumSq = 0.0;          // double: a 65536-frame span of ~1.0 samples
                                 // loses the tail of the sum in float
    for( length_t i = 0; i < n; ++i ) {
        const float s = p[i];
        const float a = s < 0.0f ? -s : s;
        if( a > peak ) peak = a;
        sumSq += (double)s * (double)s;
    }

    out.peak       = peak;
    out.meanSquare = (float)(sumSq / (double)n);
    out.frames     = (uint32_t)n;
    out.clipped    = (peak >= clipThreshold);
    return out;
}

#endif  // _TW_LEVEL_SCAN_H_
