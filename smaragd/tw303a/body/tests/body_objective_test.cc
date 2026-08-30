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
    for( double t = 0.0; t < sec - 1e-12; t += dt ) {
        twBodyMotionSample s;
        s.angVel       = A * w * std::cos( w * t );
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
              4.0, 0.01, "and FULL RANGE ONCE A SECOND is exactly 4.0 -- the"
                         " anchor maxUrge is stated as" );

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
        twBodyObjectiveParams p;                    // maxUrge 4.0
        const double halfUrge = 0.5;                // -> want 2.0 rl/s
        // Exactly meeting it: 4*f*(A/rom) = 2.0 at f = 1 Hz needs A = rom/2.
        const auto met  = sine( rom * 0.50, 1.0, 20.0, dt, halfUrge );
        const auto over = sine( rom * 1.00, 1.0, 20.0, dt, halfUrge );  // 2x
        const auto under= sine( rom * 0.25, 1.0, 20.0, dt, halfUrge );  // 0.5x
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
    // THE SECTION THAT MAKES THE SENTENCE MEAN ANYTHING. A body that thrashes
    // for a third of the window and stops has the same window MEAN as one going
    // steadily at a third the rate; scoring sub-windows separates them, and
    // separates them by exactly the urge that went unmet.
    {
        twBodyObjectiveParams p;
        const double urge = 0.5;                    // want 2.0 rl/s
        // (a) steady, meeting it the whole way.
        const auto steady = sine( rom * 0.5, 1.0, 21.0, dt, urge );
        // (b) the SAME TOTAL TRAVEL, done in the first third at treble rate.
        std::vector<twBodyMotionSample> burst;
        {
            const auto hot = sine( rom * 1.5, 1.0, 7.0, dt, urge );
            burst = hot;
            for( double t = 0.0; t < 14.0 - 1e-12; t += dt ) {
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
        near( rb.match, 7.0 / 21.0, 0.02,
              "scoring exactly the fraction of the window it was actually"
              " dancing in -- a gap costs precisely the urge it missed" );
    }

    // --- 5. GAMED ONE WAY: buying match with unsustainable effort -----------
    // The budget is what stops it, and `budgetBound` is what SAYS so. Which
    // ceiling bound is an observation, not a setting: it is the difference
    // between a body held back by the music and one held back by itself.
    {
        twBodyObjectiveParams p;
        const auto cheap = sine( rom * 0.5, 1.0, 20.0, dt, 0.5, 0.4 * mgd );
        const auto dear  = sine( rom * 0.5, 1.0, 20.0, dt, 1.0, 3.0 * mgd );
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
        const auto good = sine( rom * 0.5, 1.0, 20.0, dt, 0.5, 0.4 * mgd );
        const auto bad  = sine( rom * 0.5, 1.0, 20.0, dt, 0.5, 3.0 * mgd );
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
        std::vector<twBodyMotionSample> quiet( 20000 );   // 20 s, no urge, no motion
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
