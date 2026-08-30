#include "tw/body/twbodyobjective.h"

#include <cmath>

double twBodyDrive( double support, double tension, double coupling )
{
    // hypot() removes the resonator's phase, hence its response; the divide
    // removes the model's own coupling constant. What is left is the music.
    if( coupling <= 0.0 ) return 0.0;
    return std::sqrt( support * support + tension * tension ) / coupling;
}

double twBodyUrgeSmoother::step( double drive, double dt )
{
    if( dt > 0.0 && tauSec > 0.0 ) {
        // One pole, in the form that is exact for a constant input held over
        // the step rather than the dt/(tau+dt) approximation -- the hop grid
        // is 10 ms against a 1 s constant so the two agree to 0.005%, but a
        // caller feeding it a coarser grid should not silently get a
        // different time constant.
        const double a = 1.0 - std::exp( -dt / tauSec );
        state += a * ( drive - state );
    }
    if( reference <= 0.0 ) return 0.0;
    const double u = state / reference;
    return u < 0.0 ? 0.0 : ( u > 1.0 ? 1.0 : u );
}

double twBodyAchievedRate( const twBodyMotionSample *samples, int count,
                           double dt, double romRad )
{
    if( !samples || count <= 0 || dt <= 0.0 || romRad <= 0.0 ) return 0.0;
    double travel = 0.0;                      // radians actually covered
    for( int i = 0; i < count; i++ )
        travel += std::fabs( samples[i].angVel ) * dt;
    const double seconds = (double) count * dt;
    return ( travel / romRad ) / seconds;     // range-lengths per second
}

double twBodyEffort( const twBodyMotionSample *samples, int count,
                     double gravityMoment )
{
    if( !samples || count <= 0 || gravityMoment <= 0.0 ) return 0.0;
    double sum = 0.0;
    for( int i = 0; i < count; i++ ) {
        // TOTAL muscle torque. Counting only the active half would report a
        // body straining against a beat as cheaper than one standing leant
        // over -- see the header.
        const double tau = ( samples[i].activeTorque + samples[i].posturalTorque )
                           / gravityMoment;
        sum += tau * tau;
    }
    return sum / (double) count;
}

twBodyObjectiveResult twBodyObjective( const twBodyMotionSample *samples,
                                       int count, double dt, double romRad,
                                       double gravityMoment,
                                       const twBodyObjectiveParams &p )
{
    twBodyObjectiveResult out;
    if( !samples || count <= 0 || dt <= 0.0 || romRad <= 0.0 ) return out;

    out.achieved = twBodyAchievedRate( samples, count, dt, romRad );
    out.effort   = twBodyEffort( samples, count, gravityMoment );

    // SUSTAINMENT. Scored per sub-window and summed, never as one window mean:
    // a body that thrashes for five seconds and stops for fifteen has the same
    // mean as one going steadily, and the requester's sentence is about the
    // second. A gap therefore costs exactly the urge it failed to meet.
    int per = (int) ( p.subWindowSec / dt + 0.5 );
    if( per < 1 ) per = 1;
    double metSum = 0.0, urgeSum = 0.0, weight = 0.0;
    double weakest = 1.0;
    bool sawUrge = false;
    for( int a = 0; a < count; a += per ) {
        int b = a + per; if( b > count ) b = count;
        const int n = b - a;
        const double got = twBodyAchievedRate( samples + a, n, dt, romRad );
        double urgeNorm = 0.0;
        for( int i = a; i < b; i++ ) {
            // CLAMPED, because urgeNorm is an ABSOLUTE scale now and a track
            // louder than the reference really can exceed 1. Harmless because
            // of the ceiling below: past full urge, more urge earns nothing.
            const double u = samples[i].urgeNorm;
            urgeNorm += u < 0.0 ? 0.0 : ( u > 1.0 ? 1.0 : u );
        }
        urgeNorm /= (double) n;
        const double want = urgeNorm * p.maxUrge;
        // "as intense as it LIKES to" -- there is a ceiling and exceeding it is
        // not extra reward, which is what min() is doing and why match cannot
        // be gamed by simply moving more.
        //
        // WEIGHTED BY THE SUB-WINDOW'S ACTUAL LENGTH, which is not a rounding
        // detail. A trailing sub-window holding one sample used to be scored
        // against a WHOLE sub-window's urge, so a 24.001 s run of a 24 s window
        // lost a QUARTER of its match to a millisecond -- found by this file's
        // own sustainment case reading 0.250 where the closed form says 0.333,
        // and worth recording because the header warned about exactly this
        // failure mode while the code committed it.
        const double wt = (double) n;
        metSum  += ( got < want ? got : want ) * wt;
        urgeSum += want * wt;
        weight  += wt;
        if( want > 1e-12 ) {
            sawUrge = true;
            const double frac = ( got < want ? got : want ) / want;
            if( frac < weakest ) weakest = frac;
        }
        out.subWindows++;
    }
    out.urge  = weight > 0.0 ? urgeSum / weight : 0.0;
    out.match = urgeSum > 1e-12 ? metSum / urgeSum : 0.0;
    // Silence is not a performance, and neither is it a failure.
    out.weakestSub  = sawUrge ? weakest : 0.0;
    out.withinBudget = out.effort <= p.effortBudget;
    // WHICH CEILING BOUND, which is an observation rather than a setting: the
    // budget if the body could not keep up, the urge if the music was not
    // asking for much. Only meaningful where the body fell short at all.
    out.budgetBound = sawUrge && out.match < 0.999 && !out.withinBudget;
    return out;
}

double twBodyObjectiveScore( const twBodyObjectiveResult &r,
                             const twBodyObjectiveParams &p,
                             double lambda )
{
    // A LAGRANGE MULTIPLIER, not a weight: it prices the CONSTRAINT VIOLATION
    // only, so the optimum is independent of it once it is large enough to
    // bind. Pricing the effort itself (`match - lambda*effort`) would make the
    // answer depend on a number nobody can source, which is the form C4b was
    // first written with and the review's measure-dependence finding is about.
    const double over = r.effort - p.effortBudget;
    return r.match - ( over > 0.0 ? lambda * over : 0.0 );
}
