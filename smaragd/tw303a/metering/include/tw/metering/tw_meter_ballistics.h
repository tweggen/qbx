#ifndef _TW_METER_BALLISTICS_H_
#define _TW_METER_BALLISTICS_H_

// Proposal 34 — meter ballistics: turns a stream of twLevelSample measurements
// into the two numbers a meter draws (a fast peak with a held tick, and a slow
// RMS), plus a latching clip flag.
//
// Deliberately Qt-free and engine-state-free so it can be unit-tested without a
// widget or a graph. It lives on the UI thread, ONE INSTANCE PER METER, and is
// driven by wall-clock time rather than by tick count: every decay is expressed
// per second and integrated over the actual dt between calls. That is what
// makes the reading frame-rate independent — a 30 Hz pump, a 10 Hz pump and one
// single big step all converge to the same value, which metering_test asserts.

#include "tw/metering/tw_level_scan.h"

struct twMeterBallisticsConfig {
    float floorDb            = -60.0f;  // bottom of the scale; also the "silence" value
    float ceilDb             =  +6.0f;  // top of the scale (display mapping only)
    float decayDbPerSec      =  20.0f;  // peak fall rate
    float holdSec            =   1.5f;  // how long the peak tick stays put
    float holdDecayDbPerSec  =  12.0f;  // fall rate once the hold expires
    float rmsTauSec          =   0.3f;  // RMS integration time constant (300 ms)
    float clipThreshold      = TW_METER_CLIP_THRESHOLD;
};

class twMeterBallistics
{
public:
    explicit twMeterBallistics( const twMeterBallisticsConfig &cfg =
                                    twMeterBallisticsConfig() );

    // Feed a measurement taken at wall-clock `nowSec` (monotonic, seconds).
    // Attack is INSTANTANEOUS — a peak meter that averages its way up
    // under-reads transients, which is the one thing it exists to catch.
    void push( const twLevelSample &s, double nowSec );

    // No measurement available (transport stopped, or a page miss). Decays as
    // if the input were silent, so a dropout reads as a fall and never as a
    // frozen bar.
    void idle( double nowSec );

    // Back to floor, hold cleared, clip cleared, clock unlatched.
    void reset();

    float peakDb() const { return peakDb_; }
    float holdDb() const { return holdDb_; }
    float rmsDb()  const;

    bool clipped() const { return clipped_; }
    void clearClip()     { clipped_ = false; }

    const twMeterBallisticsConfig &config() const { return cfg_; }

    // Linear amplitude -> dBFS, with anything at or below the floor (and any
    // non-positive or non-finite input) reported AS the floor.
    static float twLinToDb( float lin, float floorDb );

private:
    // Integrate every time-dependent term from lastSec_ to nowSec.
    // `target` is what the RMS integrator moves toward this step.
    void advance_( double nowSec, float targetMeanSquare );

    twMeterBallisticsConfig cfg_;

    float  peakDb_;
    float  holdDb_;
    float  meanSquare_;    // the integrated (EMA) mean-square, linear
    bool   clipped_;

    double lastSec_;
    double holdUntilSec_;
    bool   started_;       // false until the first push/idle latches the clock
};

#endif  // _TW_METER_BALLISTICS_H_
