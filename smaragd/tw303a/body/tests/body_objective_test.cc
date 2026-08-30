// Proposal 44 C4b -- THE OBJECTIVE, in numbers.
//
// This file gates ONE SENTENCE of the requester's, and every section names the
// part of it under test:
//
//   *match is enabling the body to express its urge to move as intensely as it
//   likes to, over an extended period of time -- tens of seconds*
//
// INTENSELY -> section 1, and there is NO PHASE TERM anywhere in this file.
// AS IT LIKES TO -> section 3, the ceiling: moving more is not more reward.
// OVER AN EXTENDED PERIOD -> section 4, sustainment: a gap costs what it missed.
//
// Sections 5-7 are the three ways an objective like this gets gamed, asserted
// as failures rather than left to be discovered by a controller exploiting them.
//
// Every assertion is a CLOSED FORM or a RELATION. The two free parameters
// (maxUrge, effortBudget) are VERIFY and nothing here rests on their values --
// where one appears it is as its own stated anchor, which is a definition.

#include "tw/body/twbodyobjective.h"
#include "tw/body/twbodymeasures.h"

#include <cmath>
#include <cstdio>
#include <vector>

static int g_fail = 0;
static void check( bool ok, const char *what )
{ if( !ok ) { std::printf( "FAIL: %s\n", what ); g_fail++; } }
static void near( double got, double want, double tol, const char *what )
{ if( !( std::fabs( got - want ) <= tol ) ) {
    std::printf( "FAIL: %s -- got %.6f want %.6f (tol %.6f)\n", what, got, want, tol );
    g_fail++; } }

static const double kPi = 3.14159265358979323846;

/** A sinusoidal joint history: amplitude A rad at f Hz, for `sec` seconds,
 * with a constant urge and a constant active torque. */
static std::vector<twBodyMotionSample>
sine( double A, double f, double sec, double dt, double urge, double tau = 0.0 )
{
    std::vector<twBodyMotionSample> v;
    const double w = 2.0 * kPi * f;
    // Integer sample count, never an accumulated float: at dt = 1 ms the drift
    // over 24 s is enough to add a sample, and one extra sample used to cost a
    // quarter of the match (see twbodyobjective.cc's sub-window weighting).
    const long n = (long) ( sec / dt + 0.5 );
    for( long i = 0; i < n; i++ ) {
        twBodyMotionSample s;
        s.angVel       = A * w * std::cos( w * (double) i * dt );
        s.activeTorque = tau;
        s.urgeNorm     = urge;
        v.push_back( s );
    }
    return v;
}

int main()
{
    const double dt  = 0.001;
    const double rom = 60.0 * kPi / 180.0;      // a cervical-scale range
    const twBodyMeasures meas;
    const twBodySegment  head = meas.segment( twBodySeg::HeadNeck );
    const double mgd = head.mass * twBodyGravity * head.comFromProx;

    // --- 1. INTENSELY: achieved movement is TRAVEL, and it has a closed form
    // A sinusoid of amplitude A at f Hz covers 4*A per cycle, so
    //     achieved = 4 * f * (A / rom)   range-lengths per second.
    // Full range once a second is exactly 4.0, which is where maxUrge's anchor
    // comes from -- the parameter is a definition, not a measurement.
    {
        std::printf( "  ACHIEVED = 4*f*(A/rom) range-lengths/s:\n" );
        for( double f : { 0.5, 1.0, 2.0, 4.0 } ) {
            for( double frac : { 0.25, 0.5, 1.0 } ) {
                const auto v = sine( frac * rom, f, 8.0, dt, 0.0 );
                const double got  = twBodyAchievedRate( v.data(), (int) v.size(), dt, rom );
                const double want = 4.0 * f * frac;
                if( frac == 1.0 )
                    std::printf( "    %.1f Hz at full range -> %6.3f\n", f, got );
                near( got, want, 0.01, "achieved is the closed form 4*f*(A/rom)" );
            }
        }
        const auto full1 = sine( rom, 1.0, 8.0, dt, 0.0 );
        near( twBodyAchievedRate( full1.data(), (int) full1.size(), dt, rom ),
              4.0, 0.01, "FULL RANGE ONCE A SECOND is exactly 4.0" );
        const auto full3 = sine( rom, 3.0, 8.0, dt, 0.0 );
        near( twBodyAchievedRate( full3.data(), (int) full3.size(), dt, rom ),
              12.0, 0.01, "and THREE TIMES A SECOND exactly 12.0 -- the anchor"
                          " maxUrge is stated as" );
        // Worth knowing when reading any match below: a hard 2 Hz head-bang at
        // full range is 8.0, so at maxUrge 12 the ceiling is OFF most of the
        // time and the BUDGET is what binds. That is a consequence of the
        // requester's value, not of the metric.
        const auto bang = sine( rom, 2.0, 8.0, dt, 0.0 );
        std::printf( "    a full-range 2 Hz head-bang = %.3f, against"
                     " maxUrge %.1f\n",
                     twBodyAchievedRate( bang.data(), (int) bang.size(), dt, rom ),
                     twBodyObjectiveParams().maxUrge );

        // A BIG SLOW sway and a SMALL FAST one are not the same intensity, and
        // that is the choice this metric makes. "Hard and sudden" is the
        // requester's own phrase; peak excursion cannot express it.
        const auto slowBig   = sine( rom,       0.5, 8.0, dt, 0.0 );
        const auto fastSmall = sine( rom * 0.5, 2.0, 8.0, dt, 0.0 );
        const double aSlow = twBodyAchievedRate( slowBig.data(),   (int) slowBig.size(),   dt, rom );
        const double aFast = twBodyAchievedRate( fastSmall.data(), (int) fastSmall.size(), dt, rom );
        std::printf( "    full range at 0.5 Hz = %.3f, HALF range at 2 Hz = %.3f\n",
                     aSlow, aFast );
        check( aFast > aSlow * 1.5,
               "a small FAST movement is more intense than a big slow one at"
               " twice the excursion -- travel, not amplitude" );
    }

    // --- 1a. BODY-INDEPENDENCE, which is what the whole torque decision was for
    // A mouse and a person can both do three range-lengths a second. If the
    // objective is not invariant to body scale, section 8 question 3's
    // reasoning is void and the cross-species claim goes with it.
    {
        const auto human = sine( rom, 2.0, 8.0, dt, 0.0 );
        const double romMouse = rom;                    // same RANGE, tiny body
        const auto mouse = sine( romMouse, 2.0, 8.0, dt, 0.0 );
        near( twBodyAchievedRate( human.data(), (int) human.size(), dt, rom ),
              twBodyAchievedRate( mouse.data(), (int) mouse.size(), dt, romMouse ),
              1e-12, "achieved depends on the body only through its own ROM" );
        // Effort likewise: the same torque RELATIVE to the segment's own weight
        // moment costs the same, whatever the segment weighs.
        std::vector<twBodyMotionSample> big( 1000 ), small( 1000 );
        for( int i = 0; i < 1000; i++ ) {
            big[i].activeTorque   = 2.0 * mgd;          // 2x its own moment
            small[i].activeTorque = 2.0 * ( mgd / 40.0 );
        }
        near( twBodyEffort( big.data(), 1000, mgd ),
              twBodyEffort( small.data(), 1000, mgd / 40.0 ), 1e-12,
              "effort depends on the body only through its own gravity moment" );
        near( twBodyEffort( big.data(), 1000, mgd ), 4.0, 1e-12,
              "and is the closed form (tau/mgd)^2" );
    }

    // --- 2. EFFORT COUNTS THE POSTURAL TORQUE ------------------------------
    // Leaving it out reports a body straining against a beat as CHEAPER than
    // one merely standing leant over. It is why twBodyPosturalTorque() is
    // public, and it is the one thing in this metric that cannot be recovered
    // later from the trajectory alone.
    {
        std::vector<twBodyMotionSample> a( 500 ), b( 500 );
        for( int i = 0; i < 500; i++ ) {
            a[i].activeTorque   = mgd;  a[i].posturalTorque = 0.0;
            b[i].activeTorque   = 0.0;  b[i].posturalTorque = mgd;
        }
        near( twBodyEffort( a.data(), 500, mgd ), twBodyEffort( b.data(), 500, mgd ),
              1e-12, "HOLDING a segment up costs exactly what MOVING it costs,"
                     " newton-metre for newton-metre" );
        near( twBodyEffort( a.data(), 500, mgd ), 1.0, 1e-12,
              "and 'a torque equal to your own weight moment' is exactly effort"
              " 1.0 -- the anchor effortBudget is stated as" );
        std::vector<twBodyMotionSample> both( 500 );
        for( int i = 0; i < 500; i++ )
            { both[i].activeTorque = mgd; both[i].posturalTorque = mgd; }
        near( twBodyEffort( both.data(), 500, mgd ), 4.0, 1e-12,
              "and the two ADD before squaring -- muscle does not know which"
              " half of its own tension is postural" );
    }

    // --- 3. AS IT LIKES TO: there is a CEILING, and it is not a soft one ----
    // Moving harder than the music asks earns nothing. Without this the
    // objective is maximised by thrashing, and "as intense as IT LIKES TO"
    // becomes "as intense as possible" -- a different claim, and not the
    // requester's.
    {
        twBodyObjectiveParams p;                    // maxUrge 12.0
        const double halfUrge = 0.5;                // -> want 6.0 rl/s
        // Exactly meeting it: 4*f*(A/rom) = 6.0 at f = 3 Hz needs A = rom/2.
        const auto met  = sine( rom * 0.50, 3.0, 24.0, dt, halfUrge );
        const auto over = sine( rom * 1.00, 3.0, 24.0, dt, halfUrge );  // 2x
        const auto under= sine( rom * 0.25, 3.0, 24.0, dt, halfUrge );  // 0.5x
        const auto rm = twBodyObjective( met.data(),   (int) met.size(),   dt, rom, mgd, p );
        const auto ro = twBodyObjective( over.data(),  (int) over.size(),  dt, rom, mgd, p );
        const auto ru = twBodyObjective( under.data(), (int) under.size(), dt, rom, mgd, p );
        std::printf( "  CEILING: achieved %.2f/%.2f/%.2f rl/s against urge %.2f"
                     " -> match %.3f / %.3f / %.3f\n",
                     ru.achieved, rm.achieved, ro.achieved, rm.urge,
                     ru.match, rm.match, ro.match );
        near( rm.match, 1.0, 1e-4, "meeting the urge exactly scores 1" );
        near( ro.match, 1.0, 1e-4, "and DOUBLING it scores exactly the same --"
                                   " overshoot is not extra reward" );
        near( ru.match, 0.5, 1e-4, "while half the urge scores exactly half" );
        check( ro.achieved > rm.achieved * 1.9,
               "even though the thrashing body really did move twice as much" );
    }

    // --- 4. OVER AN EXTENDED PERIOD: a GAP costs what it missed -------------
    // THE SECTION THAT MAKES THE SENTENCE MEAN ANYTHING. A body that dances a
    // third of the window and stops has the same window MEAN as one going
    // steadily at a third the rate; scoring sub-windows separates them, and by
    // exactly the urge that went unmet.
    {
        twBodyObjectiveParams p;                    // window 24 s, sub 8 s
        const double urge = 0.5;                    // want 6.0 rl/s
        // (a) steady, meeting it the whole way: 4*f*(A/rom) = 6 at 3 Hz.
        const auto steady = sine( rom * 0.5, 3.0, 24.0, dt, urge );
        // (b) the SAME TOTAL TRAVEL, done in the first third at treble rate.
        std::vector<twBodyMotionSample> burst;
        {
            burst = sine( rom * 1.5, 3.0, 8.0, dt, urge );
            for( long i = 0; i < (long) ( 16.0 / dt + 0.5 ); i++ ) {
                twBodyMotionSample s; s.urgeNorm = urge;   // still asked for
                burst.push_back( s );
            }
        }
        const auto rs = twBodyObjective( steady.data(), (int) steady.size(), dt, rom, mgd, p );
        const auto rb = twBodyObjective( burst.data(),  (int) burst.size(),  dt, rom, mgd, p );
        std::printf( "  SUSTAINMENT: steady achieved %.3f match %.3f weakest %.3f\n"
                     "               burst  achieved %.3f match %.3f weakest %.3f\n",
                     rs.achieved, rs.match, rs.weakestSub,
                     rb.achieved, rb.match, rb.weakestSub );
        near( rs.achieved, rb.achieved, 0.02,
              "the two bodies MOVED THE SAME AMOUNT over the window" );
        check( rb.match < rs.match * 0.5,
               "but the bursting one scores far worse -- 'over an extended"
               " period' is the whole difference" );
        near( rs.match, 1.0, 1e-3, "the steady body meets the urge throughout" );
        near( rb.weakestSub, 0.0, 1e-9,
              "and the bursting one has a sub-window where it delivered NOTHING" );
        near( rb.match, 1.0 / 3.0, 0.02,
              "scoring exactly the fraction of the window it was actually"
              " dancing in -- a gap costs precisely the urge it missed" );
        near( (double) rs.subWindows, 3.0, 0.0,
              "and the window divides into a whole number of sub-windows, so"
              " no ragged tail is scored against a full sub-window's urge" );
    }

    // --- 4a. WHY 8 SECONDS: PHRASING IS FREE, SITTING OUT A PHRASE IS NOT ---
    // The requester's sub-window is 8 s, which at 120 BPM is FOUR BARS -- a
    // phrase, not a bar and not a beat. That scale is a claim about dancing and
    // this is the pair that gates it: a dancer who pauses two seconds in every
    // eight is sustaining perfectly; one who sits out a whole phrase is not.
    // At a 1 s sub-window the first would score 0.75 and ordinary phrasing
    // would read as failure, so the two readings are separated here rather
    // than left to be discovered.
    {
        twBodyObjectiveParams p;
        const double urge = 0.5;                    // want 6.0 rl/s
        // Dances 6 s of every 8, hard enough that the 8 s block still meets the
        // urge: 8*6.0/6 = 8.0 rl/s while moving, i.e. 4*f*(A/rom) = 8 at 3 Hz.
        std::vector<twBodyMotionSample> phrased;
        for( int blk = 0; blk < 3; blk++ ) {
            const auto on = sine( rom * ( 8.0 / 12.0 ), 3.0, 6.0, dt, urge );
            phrased.insert( phrased.end(), on.begin(), on.end() );
            for( long i = 0; i < (long) ( 2.0 / dt + 0.5 ); i++ ) {
                twBodyMotionSample s; s.urgeNorm = urge;
                phrased.push_back( s );
            }
        }
        const auto rp = twBodyObjective( phrased.data(), (int) phrased.size(),
                                         dt, rom, mgd, p );
        twBodyObjectiveParams beat = p; beat.subWindowSec = 1.0;
        const auto rbeat = twBodyObjective( phrased.data(), (int) phrased.size(),
                                            dt, rom, mgd, beat );
        std::printf( "  PHRASING (dances 6 s of every 8): match %.3f at an 8 s"
                     " sub-window, %.3f at a 1 s one\n", rp.match, rbeat.match );
        near( rp.match, 1.0, 1e-3,
              "AT 8 SECONDS, PAUSING WITHIN A PHRASE IS FREE" );
        near( rbeat.match, 0.75, 0.01,
              "at 1 second the SAME dancer reads 0.75 -- which is why the"
              " sub-window length is a claim about dancing, not a tolerance" );
        check( rp.weakestSub > 0.99,
               "and no phrase was under-delivered" );
    }

    // --- 4b. A RAGGED TAIL COSTS ONLY WHAT IT IS -- found the hard way -------
    // A trailing sub-window shorter than the rest used to be scored against a
    // WHOLE sub-window's urge, so a 24.001 s run of a 24 s window lost A
    // QUARTER of its match to one millisecond. The header had warned about
    // exactly this failure mode while the code committed it; it was caught by
    // section 4 reading 0.250 where the closed form says 0.333.
    //
    // It is not an edge case waiting to happen -- a real hop grid will almost
    // never divide a window evenly, so EVERY run would have paid it.
    {
        twBodyObjectiveParams p;
        const double urge = 0.5;
        const auto whole = sine( rom * 0.5, 3.0, 24.0,  dt, urge );
        auto ragged = whole;
        // The tail must UNDER-deliver, or the defect is invisible: a tail that
        // meets its urge scores 1 whether it is weighted or not, which is how
        // a first version of this section passed under its own sabotage.
        for( int i = 0; i < 5; i++ ) {              // 5 ms of nothing
            twBodyMotionSample x; x.urgeNorm = urge;
            ragged.push_back( x );
        }
        const auto rw = twBodyObjective( whole.data(),  (int) whole.size(),  dt, rom, mgd, p );
        const auto rr = twBodyObjective( ragged.data(), (int) ragged.size(), dt, rom, mgd, p );
        std::printf( "  RAGGED TAIL: 24.000 s -> match %.4f (%d sub-windows);"
                     " 24.005 s -> %.4f (%d)\n",
                     rw.match, rw.subWindows, rr.match, rr.subWindows );
        check( rr.subWindows == rw.subWindows + 1,
               "five extra milliseconds really do open a new sub-window" );
        near( rr.match, rw.match, 1e-3,
               "AND COST ALMOST NOTHING -- a sub-window is worth its own"
               " LENGTH, not one whole share of the window" );
        check( rr.match > 0.99,
               "specifically: five silent milliseconds do not cost a QUARTER of"
               " the run, which is what an unweighted sub-window charges" );
    }

    // --- 4c. AN ABSOLUTE URGE SCALE CAN EXCEED 1, AND IS CLAMPED ------------
    // urgeNorm was per-track at first, so values above 1 were impossible BY
    // CONSTRUCTION. The requester's decision (2026-08-30) made it an ABSOLUTE
    // scale comparable across tracks -- which is the whole point, since "this
    // music demands more movement than that music" is the claim proposal 44
    // exists to test -- and a track louder than the reference now produces
    // them. Unclamped, such a track would demand a superhuman rate and every
    // body would read as failing it.
    {
        twBodyObjectiveParams p;                    // maxUrge 12.0
        // A body moving at exactly full urge, under a track asking for 3x it.
        const auto v = sine( rom * 1.0, 3.0, 24.0, dt, 3.0 );   // achieved 12.0
        const auto r = twBodyObjective( v.data(), (int) v.size(), dt, rom, mgd, p );
        std::printf( "  ABSOLUTE URGE: track asks urgeNorm 3.0, body delivers"
                     " %.2f rl/s -> urge %.2f, match %.3f\n",
                     r.achieved, r.urge, r.match );
        near( r.urge, p.maxUrge, 1e-9,
              "urgeNorm above 1 is CLAMPED -- maxUrge is the ceiling on what"
              " any track may ask for" );
        near( r.match, 1.0, 1e-3,
              "so a body at full urge scores 1 against a louder-than-reference"
              " track, rather than being charged for the excess" );
    }

    // --- 4d. WHERE urgeNorm COMES FROM (C4b's last modelling claim) ---------
    // The three obvious sources are all wrong the same way: perUnitPower and
    // dissip are the resonator's RESPONSE, so using either would make urge and
    // achieved the same quantity and match trivially 1. What survives is the
    // DRIVE, recovered from the two "groove.dyn" phase channels.
    {
        // hypot(support,tension) is |k*F|, so the phase -- and with it the
        // response -- divides out. Any phase, same answer.
        const double kF = 0.03;
        for( double phi : { 0.0, 0.7, 1.9, 3.0, -2.2 } )
            near( twBodyDrive( kF * std::cos( phi ), kF * std::sin( phi ), 1.5 ),
                  kF / 1.5, 1e-12,
                  "the drive is the MAGNITUDE -- the resonator's phase, and so"
                  " its response, divides out" );

        // AND k IS OURS, F IS THE MUSIC. Measured on a0_broadband_grid: the
        // raw p99 drive reads 0.02896 for sway (k 1.5) and 0.06757 for twobar
        // (k 3.5) -- twice as much, purely because of a model constant. The
        // divide collapses them onto the same number, and these two literals
        // are that measurement.
        const double sway   = twBodyDrive( 0.02896, 0.0, 1.5 );
        const double twobar = twBodyDrive( 0.06757, 0.0, 3.5 );
        std::printf( "  DRIVE: sway raw 0.02896 / k 1.5 = %.5f;"
                     " twobar raw 0.06757 / k 3.5 = %.5f\n", sway, twobar );
        near( twobar / sway, 1.0, 0.01,
              "MEASURED: two units whose raw drives differ 2.3x report the SAME"
              " music once their couplings are divided out" );
        near( twBodyDrive( 1.0, 0.0, 0.0 ), 0.0, 0.0,
              "and a zero coupling is refused rather than dividing by it" );
    }

    // --- 4e. URGE IS A LEVEL, NOT AN ONSET TRAIN ---------------------------
    // Raw F is a half-wave-rectified envelope difference: measured, its MEDIAN
    // over every committed fixture is exactly 0.00000, because it is zero
    // between onsets. A per-hop urge would be zero nearly always and every
    // window mean would be meaningless.
    {
        const double dt = 0.001;
        // Step response: a one-pole reaches 1 - 1/e of its target in exactly
        // one time constant, and that is what says tau is what it claims.
        {
            twBodyUrgeSmoother sm;
            sm.reference = 1.0;                 // read the raw smoothed value
            double out = 0.0;
            for( double t = 0.0; t < sm.tauSec - 1e-9; t += dt )
                out = sm.step( 1.0, dt );
            near( out, 1.0 - std::exp( -1.0 ), 1e-3,
                  "one time constant reaches exactly 1 - 1/e" );
        }
        // An ONSET TRAIN of duty d and height A smooths to A*d -- so a sparse
        // loud track and a dense quiet one with the same energy read alike,
        // which is what makes urge a property of the passage rather than of
        // whichever hop you sampled.
        {
            twBodyUrgeSmoother sm; sm.reference = 1.0;
            const double A = 0.02, period = 0.25, duty = 0.04;   // 4 Hz onsets
            double lo = 1e30, hi = -1e30, sum = 0.0; int n = 0;
            for( double t = 0.0; t < 12.0; t += dt ) {
                const double out =
                    sm.step( std::fmod( t, period ) < duty * period ? A : 0.0, dt );
                if( t >= 11.0 ) {           // one settled second
                    lo = std::min( lo, out ); hi = std::max( hi, out );
                    sum += out; n++;
                }
            }
            const double mean = sum / (double) n;
            const double ripple = ( hi - lo ) / mean;
            std::printf( "  ONSET TRAIN: A %.3f at duty %.2f -> mean %.5f"
                         " (closed form %.5f), ripple %.1f%% of mean\n",
                         A, duty, mean, A * duty, 100.0 * ripple );
            near( mean, A * duty, A * duty * 0.02,
                  "an onset train smooths to its own DUTY-WEIGHTED MEAN" );
            // THE RIPPLE IS REAL AND IS WHY tau MUST EXCEED THE ONSET PERIOD.
            // A single instantaneous sample is NOT the mean -- asserting that
            // was this section's first version, and it failed 11% low because
            // the loop happened to end in the gap between onsets. At tau 1 s
            // against a 0.25 s period the swing is exp(-0.25) either side.
            check( ripple > 0.15 && ripple < 0.35,
                   "and RIPPLES by roughly exp(-period/tau) -- which is why a"
                   " single sample of urge is not its level" );
        }
        // The normalisation and its clamp, against the calibration constant.
        {
            twBodyUrgeSmoother sm;              // reference 0.008
            double out = 0.0;
            for( double t = 0.0; t < 12.0; t += dt ) out = sm.step( 0.004, dt );
            near( out, 0.5, 1e-3, "half the reference drive is urgeNorm 0.5" );
            twBodyUrgeSmoother hot;
            for( double t = 0.0; t < 12.0; t += dt ) out = hot.step( 0.040, dt );
            near( out, 1.0, 0.0,
                  "and a track FIVE TIMES the reference clamps at 1 -- which an"
                  " absolute scale makes reachable and the ceiling makes"
                  " harmless" );
        }
        // What the committed fixtures actually read, so the calibration is
        // visible rather than buried: measured smoothed p99 drives, 0.00030 to
        // 0.00262 across 30 unit/fixture pairs.
        {
            twBodyUrgeSmoother lo, hi;
            double a = 0.0, b = 0.0;
            for( double t = 0.0; t < 12.0; t += dt ) {
                a = lo.step( 0.00030, dt );
                b = hi.step( 0.00262, dt );
            }
            std::printf( "  CALIBRATION: the committed fixtures span urgeNorm"
                         " %.3f to %.3f against reference %.4f\n",
                         a, b, twBodyUrgeReference );
            check( a > 0.0 && b < 1.0,
                   "the committed fixtures sit INSIDE the scale -- neither"
                   " floored at nothing nor clamped at the ceiling" );
            check( b > 0.2,
                   "and high enough to be a usable fraction of it" );
        }
    }

    // --- 5. GAMED ONE WAY: buying match with unsustainable effort -----------
    // The budget is what stops it, and `budgetBound` is what SAYS so. Which
    // ceiling bound is an observation, not a setting: it is the difference
    // between a body held back by the music and one held back by itself.
    {
        twBodyObjectiveParams p;
        const auto cheap = sine( rom * 0.5, 3.0, 24.0, dt, 0.5, 0.4 * mgd );
        const auto dear  = sine( rom * 0.5, 3.0, 24.0, dt, 1.0, 3.0 * mgd );
        const auto rc = twBodyObjective( cheap.data(), (int) cheap.size(), dt, rom, mgd, p );
        const auto rd = twBodyObjective( dear.data(),  (int) dear.size(),  dt, rom, mgd, p );
        std::printf( "  BUDGET: effort %.3f (within) vs %.3f (over) -> score"
                     " %.3f vs %.3f\n", rc.effort, rd.effort,
                     twBodyObjectiveScore( rc, p ), twBodyObjectiveScore( rd, p ) );
        check( rc.withinBudget && !rd.withinBudget,
               "a body over its budget is reported as over it" );
        check( rd.budgetBound,
               "and when it ALSO fell short, the budget is named as the reason" );
        check( !rc.budgetBound,
               "while a body inside its budget was limited by the music" );
        check( twBodyObjectiveScore( rd, p ) < twBodyObjectiveScore( rc, p ),
               "and the score prefers the sustainable performance" );
    }

    // --- 6. LAMBDA IS A MULTIPLIER, NOT A TASTE KNOB -----------------------
    // The substantive difference between this objective and the
    // "match - lambda*effort" form C4b was first written with, and the reason
    // the review's measure-dependence finding does not reach it: pricing the
    // CONSTRAINT VIOLATION rather than the effort itself makes the ranking
    // independent of lambda once it is large enough to bind. A gate can sweep
    // it; nobody has to source it.
    {
        twBodyObjectiveParams p;
        const auto good = sine( rom * 0.5, 3.0, 24.0, dt, 0.5, 0.4 * mgd );
        const auto bad  = sine( rom * 0.5, 3.0, 24.0, dt, 0.5, 3.0 * mgd );
        const auto rg = twBodyObjective( good.data(), (int) good.size(), dt, rom, mgd, p );
        const auto rb = twBodyObjective( bad.data(),  (int) bad.size(),  dt, rom, mgd, p );
        near( twBodyObjectiveScore( rg, p, 1.0 ),
              twBodyObjectiveScore( rg, p, 1000.0 ), 0.0,
              "lambda does not touch a performance INSIDE the budget at all" );
        for( double lam : { 1.0, 4.0, 40.0, 400.0 } )
            check( twBodyObjectiveScore( rg, p, lam ) > twBodyObjectiveScore( rb, p, lam ),
                   "and the ORDERING is the same at every lambda that binds --"
                   " so the optimum does not depend on a number nobody can source" );
    }

    // --- 7. GAMED THE OTHER WAY: silence ------------------------------------
    // A body that does nothing while the music asks for nothing must not score
    // 1 -- that would make standing still through a quiet passage the optimal
    // performance and would dominate any long window with silence in it.
    {
        twBodyObjectiveParams p;
        std::vector<twBodyMotionSample> quiet( 24000 );   // 24 s, no urge, no motion
        const auto rq = twBodyObjective( quiet.data(), (int) quiet.size(), dt, rom, mgd, p );
        near( rq.match, 0.0, 0.0, "SILENCE IS NOT A PERFORMANCE -- match 0" );
        near( rq.weakestSub, 0.0, 0.0, "nor a failure to be blamed on the body" );
        check( !rq.budgetBound, "and nothing bound it" );
        check( rq.withinBudget, "though it certainly stayed inside its budget" );
    }

    if( g_fail == 0 ) std::printf( "body_objective_test: PASS\n" );
    else              std::printf( "body_objective_test: %d FAILURES\n", g_fail );
    return g_fail ? 1 : 0;
}
