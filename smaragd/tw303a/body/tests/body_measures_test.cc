// Proposal 44 C2 -- the measures.
//
// WHAT THESE TESTS DO AND DO NOT PROVE. They gate the ARITHMETIC and guard
// against an accidental change to the table: the closed forms, the
// parallel-axis composition, the amplitude dependence of the crossover, and
// monotone scaling in M and H. They do NOT check the table against the
// literature -- AC2.1 is NOT met (see twbodymeasures.cc's header for what was
// searched and refused), so there is no verified range to assert against and
// asserting an unverified one is what produced proposal 44's unresolvable
// arm-frequency gate in the first place.
//
// When AC2.1 IS finished, the recorded values below will move if any constant
// moves. That is the point: they are a regression net under provisional data,
// not a validation of it.

#include "tw/body/twbodymeasures.h"

#include <cmath>
#include <cstdio>

static int g_fail = 0;

static void check( bool ok, const char *what )
{
    if( !ok ) { std::printf( "FAIL: %s\n", what ); g_fail++; }
}

static void near( double got, double want, double tol, const char *what )
{
    if( !( std::fabs( got - want ) <= tol ) ) {
        std::printf( "FAIL: %s -- got %.6f want %.6f (tol %.6f)\n",
                     what, got, want, tol );
        g_fail++;
    }
}

int main()
{
    const twBodyMeasures ref;           // 75 kg, 1.75 m
    const double kPi = 3.14159265358979323846;
    const double d10 = 10.0 * kPi / 180.0;
    const double d20 = 20.0 * kPi / 180.0;

    // --- 1. the table scales as declared ---------------------------------
    {
        const twBodySegment h = ref.segment( twBodySeg::HeadNeck );
        near( h.mass,   0.069 * 75.0, 1e-9, "head mass = massFrac*M" );
        near( h.length, 0.130 * 1.75, 1e-9, "head length = lenFrac*H" );
        near( h.comFromProx, 0.5 * h.length, 1e-9, "head CoM = comFrac*len" );
        // parallel axis, spelled out here so a change to the formula fails
        near( h.inertiaProx,
              h.mass * ( h.radiusGyrCom * h.radiusGyrCom
                         + h.comFromProx * h.comFromProx ),
              1e-12, "I_prox is the parallel-axis composition" );
    }

    // --- 2. AC2.3: the crossover, at the amplitudes that produce it -------
    // These are the two numbers proposal 44's verification note quotes, and
    // they are reproduced here to 0.01 Hz. NOTE THE AMPLITUDES: the crossover
    // depends on them through sin(theta)/theta, so a crossover quoted without
    // one is unreproducible -- which the plan's first draft did.
    {
        const double head  = twBodyCrossoverHz( ref.segment( twBodySeg::HeadNeck ), d10 );
        const double trunk = twBodyCrossoverHz( ref.segment( twBodySeg::Trunk ),    d20 );
        near( head,  1.264, 0.01, "head crossover at 10 deg" );
        near( trunk, 0.800, 0.01, "trunk crossover at 20 deg" );
        std::printf( "  head f_cross(10 deg) = %.4f Hz\n", head );
        std::printf( "  trunk f_cross(20 deg) = %.4f Hz\n", trunk );

        // Amplitude dependence is REAL and one-directional: sin(t)/t falls with
        // t, so a bigger excursion crosses over LOWER.
        check( twBodyCrossoverHz( ref.segment( twBodySeg::Trunk ), d10 )
               > twBodyCrossoverHz( ref.segment( twBodySeg::Trunk ), d20 ),
               "a larger amplitude lowers the crossover" );
    }

    // --- 3. AC2.2: the closed-form pendulum frequencies -------------------
    // RECORDED, NOT VALIDATED. Proposal 44 section 4 quotes 0.9-1.1 Hz for
    // "the arm", flagged VERIFY, while defining the arm as upper arm + forearm
    // + hand. That segment's closed form over this table is 0.7346 Hz; the
    // UPPER ARM ALONE is 1.0332 Hz. The two differ by 40%, which is why the
    // milestone must say WHICH pendulum it means before any range can be
    // asserted. Until AC2.1 is finished these are regression pins.
    {
        const twBodySegment fullArm = ref.compound( twBodySeg::UpperArm, 3 );
        const twBodySegment upper   = ref.segment( twBodySeg::UpperArm );
        const double fFull  = twBodyPendulumHz( fullArm );
        const double fUpper = twBodyPendulumHz( upper );
        std::printf( "  full arm (ua+fa+hand): m=%.3f kg d=%.4f m I=%.5f f=%.4f Hz\n",
                     fullArm.mass, fullArm.comFromProx, fullArm.inertiaProx, fFull );
        std::printf( "  upper arm alone:       m=%.3f kg d=%.4f m I=%.5f f=%.4f Hz\n",
                     upper.mass, upper.comFromProx, upper.inertiaProx, fUpper );
        near( fFull,  0.7346, 0.002, "full-arm pendulum (RECORDED, not validated)" );
        near( fUpper, 1.0332, 0.002, "upper-arm pendulum (RECORDED, not validated)" );
        check( fFull < fUpper,
               "a longer compound pendulum is slower -- the sanity check that "
               "makes the 40% gap believable rather than a transcription slip" );
    }

    // --- 4. the compound composition is self-consistent -------------------
    {
        const twBodySegment c = ref.compound( twBodySeg::UpperArm, 3 );
        const twBodySegment u = ref.segment( twBodySeg::UpperArm );
        const twBodySegment f = ref.segment( twBodySeg::Forearm );
        const twBodySegment h = ref.segment( twBodySeg::Hand );
        near( c.mass, u.mass + f.mass + h.mass, 1e-12, "compound mass is the sum" );
        near( c.length, u.length + f.length + h.length, 1e-12, "compound length is the sum" );
        // The struct's own invariant must hold for a compound too.
        near( c.inertiaProx,
              c.mass * ( c.radiusGyrCom * c.radiusGyrCom + c.comFromProx * c.comFromProx ),
              1e-9, "compound I_prox matches its own r_gyr and CoM" );
        // A one-segment compound IS that segment.
        const twBodySegment one = ref.compound( twBodySeg::Trunk, 1 );
        const twBodySegment t   = ref.segment( twBodySeg::Trunk );
        near( one.inertiaProx, t.inertiaProx, 1e-12, "compound(x,1) == segment(x)" );
    }

    // --- 5. AC2.4: scaling is exercised, not assumed ----------------------
    {
        const twBodyMeasures small{ 50.0, 1.55 };
        const twBodyMeasures large{ 110.0, 2.00 };
        for( int i = 0; i < (int) twBodySeg::Count; i++ ) {
            const twBodySeg s = (twBodySeg) i;
            check( small.segment( s ).mass   < ref.segment( s ).mass
                   && ref.segment( s ).mass  < large.segment( s ).mass,
                   "segment mass is monotone in M" );
            check( small.segment( s ).length < ref.segment( s ).length
                   && ref.segment( s ).length < large.segment( s ).length,
                   "segment length is monotone in H" );
        }
        // A taller body is a SLOWER pendulum: f ~ sqrt(g*d/(r^2+d^2)) scales as
        // 1/sqrt(H) and does not depend on M at all. Both are asserted, and the
        // mass-independence is the sharper of the two.
        const twBodySegment aRef   = ref.compound( twBodySeg::UpperArm, 3 );
        const twBodySegment aLarge = large.compound( twBodySeg::UpperArm, 3 );
        check( twBodyPendulumHz( aLarge ) < twBodyPendulumHz( aRef ),
               "a taller body's arm swings slower" );
        const twBodyMeasures heavyOnly{ 110.0, 1.75 };
        near( twBodyPendulumHz( heavyOnly.compound( twBodySeg::UpperArm, 3 ) ),
              twBodyPendulumHz( aRef ), 1e-9,
              "pendulum frequency is INDEPENDENT of body mass" );
        // 1/sqrt(H) exactly, since every length scales together.
        near( twBodyPendulumHz( aLarge ),
              twBodyPendulumHz( aRef ) * std::sqrt( 1.75 / 2.00 ), 1e-9,
              "pendulum frequency scales as 1/sqrt(H)" );
    }

    // --- 6. degenerate inputs answer, and answer zero --------------------
    {
        twBodySegment empty;
        near( twBodyPendulumHz( empty ), 0.0, 0.0, "empty segment -> 0 Hz" );
        near( twBodyCrossoverHz( empty, d10 ), 0.0, 0.0, "empty segment -> 0 crossover" );
        near( twBodyCrossoverHz( ref.segment( twBodySeg::Trunk ), 0.0 ), 0.0, 0.0,
              "zero amplitude -> 0 crossover, never a divide by zero" );
        check( ref.compound( twBodySeg::UpperArm, 0 ).mass == 0.0,
               "compound of zero segments is empty" );
        check( ref.compound( twBodySeg::Foot, 5 ).mass > 0.0,
               "compound past the end clamps rather than reading out of bounds" );
    }

    if( g_fail == 0 ) std::printf( "body_measures_test: PASS\n" );
    else              std::printf( "body_measures_test: %d FAILURES\n", g_fail );
    return g_fail ? 1 : 0;
}
