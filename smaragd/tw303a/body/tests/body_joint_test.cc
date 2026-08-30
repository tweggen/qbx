// Proposal 44 C4 (first half) -- a joint with inertia.
//
// THE CENTRAL TEST IS THE REQUESTER'S OWN, and it is section 3 below: move the
// torso forward, then STOP; the head must keep going and nod FORWARD, then ring
// down. Section 4 is its control -- the same input through the FIRST-ORDER lag
// the model shipped, which can never go forward at all. Those two together are
// the whole argument, stated as a pair so neither can pass by accident.
//
// Every stiffness here rests on tw_body's UNSOURCED constants (AC2.1 is open),
// so a natural frequency is arithmetic over provisional data. The tests are
// therefore written to assert SIGNS, RELATIONS and CONSERVATION -- things that
// hold whatever the constants turn out to be -- and only pin a magnitude where
// it is a closed form of the inputs.

#include "tw/body/twbodyjoint.h"
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

/** The neck: the head segment on a near-spherical joint, levered off the trunk. */
static twBodyJoint neckJoint( double zeta = 0.30, double ks = 2.0 )
{
    const twBodyMeasures m;
    twBodyJoint j;
    j.segment          = m.segment( twBodySeg::HeadNeck );
    j.freeAxes         = twBodySpherical();
    j.dampingRatio     = zeta;
    j.stiffnessScale   = ks;
    // The head's mass sits ABOVE the atlanto-occipital joint, so gravity is
    // DESTABILISING here. Passive tissue must exceed the gravity moment for an
    // upright head to be stable at all -- which is why ks defaults above 1.
    j.invertedPendulum = true;
    j.parentLever      = m.segment( twBodySeg::Trunk ).length;   // hip -> neck
    return j;
}

/** The requester's trunk motion: flex forward over `ramp` seconds, then STOP.
 * Smooth-start so the ramp itself is not an impulse -- the only abrupt event
 * is the arrival, which is the thing under test. */
static double trunkPhi( double t, double ramp, double amp )
{
    if( t >= ramp ) return amp;
    const double u = t / ramp;
    return amp * ( u - std::sin( 2.0 * kPi * u ) / ( 2.0 * kPi ) );
}
static double trunkPhiDD( double t, double ramp, double amp )
{
    const double h = 1e-5;
    return ( trunkPhi( t + h, ramp, amp ) - 2.0 * trunkPhi( t, ramp, amp )
             + trunkPhi( t - h, ramp, amp ) ) / ( h * h );
}

int main()
{
    const double dt = 0.0002;

    // --- 1. the integrator CONSERVES, which is what catches an integrator bug
    {
        twBodyJoint j = neckJoint( 0.0 );        // no damping, no drive
        twBodyJointState st;
        st.angle[(int) twBodyAxis::Flex] = 0.2;
        const double e0 = twBodyJointEnergy( j, st, twBodyAxis::Flex );
        const double zero[3] = { 0.0, 0.0, 0.0 };
        double eMin = e0, eMax = e0;
        for( long n = 0; n < (long) ( 60.0 / dt ); n++ ) {
            twBodyJointStep( j, st, zero, nullptr, dt );
            const double e = twBodyJointEnergy( j, st, twBodyAxis::Flex );
            eMin = std::min( eMin, e ); eMax = std::max( eMax, e );
        }
        const double drift = std::fabs( twBodyJointEnergy( j, st, twBodyAxis::Flex ) - e0 ) / e0;
        std::printf( "  60 s undamped: energy drift %.3e, band [%.6f, %.6f] J\n",
                     drift, eMin, eMax );
        check( drift < 1e-3, "undamped energy is conserved over 60 s (< 0.1%)" );
        check( ( eMax - eMin ) / e0 < 1e-3, "and pointwise too -- the step is EXACT" );
    }

    // --- 2. the natural frequency is the closed form ----------------------
    {
        const twBodyJoint j = neckJoint();
        const twBodyMeasures m;
        const twBodySegment h = m.segment( twBodySeg::HeadNeck );
        // ks = 2 passive, minus 1 for the inverted gravity moment -> 1x m*g*d.
        const double want = std::sqrt( ( 2.0 - 1.0 ) * h.mass * twBodyGravity
                                       * h.comFromProx / h.inertiaProx ) / ( 2.0 * kPi );
        near( twBodyJointNaturalHz( j, twBodyAxis::Flex ), want, 1e-9,
              "natural frequency is sqrt(k_total/I)/2pi" );
        std::printf( "  neck natural frequency (passive 2x, inverted) = %.4f Hz\n", want );

        // BOTH SIDES OF THE STABILITY THRESHOLD. An inverted segment whose
        // passive tissue does not exceed its own gravity moment cannot hold
        // itself up, and the model must say so rather than quietly taking the
        // square root of a negative number.
        twBodyJoint weak = neckJoint( 0.3, 0.6 );      // passive < gravity
        check( twBodyJointStiffness( weak ) < 0.0,
               "an inverted joint with passive stiffness below its gravity moment"
               " has NEGATIVE total stiffness" );
        check( twBodyJointNaturalHz( weak, twBodyAxis::Flex ) == 0.0,
               "and reports no natural frequency rather than a fictitious one" );
        twBodyJoint arm;
        const twBodyMeasures mm;
        arm.segment = mm.compound( twBodySeg::UpperArm, 3 );
        arm.invertedPendulum = false;                  // an arm HANGS
        arm.stiffnessScale   = 0.0;                    // no passive tissue at all
        check( twBodyJointStiffness( arm ) > 0.0,
               "a HANGING segment is stable on gravity alone -- opposite sign" );
    }

    // --- 3. THE REQUESTER'S TEST ------------------------------------------
    // Move the torso forward, then stop. The head has momentum; it must keep
    // going forward, i.e. the relative angle must go POSITIVE, then ring down.
    double fwdSecond = 0.0;
    {
        twBodyJoint j = neckJoint( 0.30 );
        twBodyJointState st;
        const double ramp = 0.25, amp = 25.0 * kPi / 180.0;
        std::vector<double> rel;
        for( double t = 0.0; t < 1.5; t += dt ) {
            const double acc[3] = { trunkPhiDD( t, ramp, amp ), 0.0, 0.0 };
            twBodyJointStep( j, st, acc, nullptr, dt );
            rel.push_back( st.angle[(int) twBodyAxis::Flex] );
        }
        double mx = 0.0; for( double v : rel ) mx = std::max( mx, v );
        fwdSecond = mx;
        int crossings = 0;
        for( size_t i = 1; i < rel.size(); i++ )
            if( rel[i-1] * rel[i] < 0.0 ) crossings++;
        std::printf( "  torso flexes 25 deg then STOPS: head forward overshoot"
                     " %.3f deg, %d zero crossings\n", mx * 180.0 / kPi, crossings );
        check( mx > 0.01, "THE HEAD NODS FORWARD after the torso stops" );
        check( crossings >= 2, "and it RINGS -- an overshoot, not a drift" );
    }

    // --- 4. THE CONTROL: the same input through a FIRST-ORDER lag ----------
    // This is what shipped. It is not that the lag nods too little; it CANNOT
    // nod at all, at any tau, because it has no state that carries motion past
    // a stop. Asserting that here is what stops anyone "fixing" the nod by
    // tuning a first-order gain.
    {
        const double ramp = 0.25, amp = 25.0 * kPi / 180.0;
        double best = -1e9;
        for( double tau : { 0.03, 0.126, 0.5 } ) {
            double y = 0.0, mx = -1e9;
            for( double t = 0.0; t < 1.5; t += dt ) {
                const double phi = trunkPhi( t, ramp, amp );
                y += dt * ( phi - y ) / tau;
                mx = std::max( mx, y - phi );        // relative angle
            }
            best = std::max( best, mx );
        }
        std::printf( "  first-order lag, best over tau in {0.03,0.126,0.5}:"
                     " max forward %.6f deg\n", best * 180.0 / kPi );
        check( best <= 1e-9, "a FIRST-ORDER lag never overshoots, at any tau" );
        check( fwdSecond > 100.0 * std::max( best, 1e-12 ),
               "the second-order joint overshoots by orders of magnitude more" );
    }

    // --- 5. ARREST: once the drive stops, it comes to rest -----------------
    {
        twBodyJoint j = neckJoint( 0.30 );
        twBodyJointState st;
        const double ramp = 0.25, amp = 25.0 * kPi / 180.0;
        double peak = 0.0; double t = 0.0;
        for( ; t < 0.6; t += dt ) {
            const double acc[3] = { trunkPhiDD( t, ramp, amp ), 0.0, 0.0 };
            twBodyJointStep( j, st, acc, nullptr, dt );
            peak = std::max( peak, std::fabs( st.angle[(int) twBodyAxis::Flex] ) );
        }
        const double zero[3] = { 0.0, 0.0, 0.0 };
        const double cycles = 6.0 / std::max( 1e-6, twBodyJointNaturalHz( j, twBodyAxis::Flex ) );
        for( double u = 0.0; u < cycles; u += dt ) twBodyJointStep( j, st, zero, nullptr, dt );
        const double left = std::fabs( st.angle[(int) twBodyAxis::Flex] ) / peak;
        std::printf( "  arrest: %.4f%% of peak left after 6 natural periods\n", 100.0 * left );
        check( left < 0.05, "a damped joint ARRESTS -- under 5%% of peak after 6 periods" );
    }

    // --- 6. THE DOF CONSTRAINT: a hinge is a hinge ------------------------
    // A constrained axis does not move, whatever drives it. Not "very stiff" --
    // zero. This is the rule that makes a knee different from a tightened ball.
    {
        twBodyJoint j = neckJoint();
        j.freeAxes = twBodyHinge( twBodyAxis::Flex );      // sagittal only
        twBodyJointState st;
        const double acc[3] = { 40.0, 40.0, 40.0 };        // drive ALL three hard
        for( long n = 0; n < 5000; n++ ) twBodyJointStep( j, st, acc, nullptr, dt );
        check( std::fabs( st.angle[(int) twBodyAxis::Flex] ) > 1e-4,
               "the FREE axis of a hinge moves" );
        near( st.angle[(int) twBodyAxis::Lateral], 0.0, 0.0, "a hinge does not move laterally" );
        near( st.angle[(int) twBodyAxis::Axial],   0.0, 0.0, "a hinge does not twist" );
        near( st.vel[(int) twBodyAxis::Lateral],   0.0, 0.0, "nor accumulate lateral velocity" );
        check( twBodyJointNaturalHz( j, twBodyAxis::Axial ) == 0.0,
               "a constrained axis reports no natural frequency" );
    }

    // --- 7. RANGE OF MOTION: the stop is dissipative ----------------------
    {
        twBodyJoint j = neckJoint( 0.05 );
        j.limitRad[(int) twBodyAxis::Flex] = 15.0 * kPi / 180.0;
        twBodyJointState st;
        const double acc[3] = { 60.0, 0.0, 0.0 };
        double worst = 0.0;
        for( long n = 0; n < 20000; n++ ) {
            twBodyJointStep( j, st, acc, nullptr, dt );
            worst = std::max( worst, std::fabs( st.angle[(int) twBodyAxis::Flex] ) );
        }
        std::printf( "  range of motion: worst |angle| %.4f deg against a 15.0 limit,"
                     " %u hits\n", worst * 180.0 / kPi, st.limitHits );
        check( worst <= 15.0 * kPi / 180.0 + 1e-12, "the joint never exceeds its limit" );
        check( st.limitHits > 0, "and the stop was actually reached" );
    }

    // --- 8. an OVERDAMPED joint cannot overshoot --------------------------
    // The damping ratio is the knob that decides whether a nod exists at all,
    // so both sides of it are asserted rather than one.
    {
        twBodyJoint j = neckJoint( 1.4 );
        twBodyJointState st;
        const double ramp = 0.25, amp = 25.0 * kPi / 180.0;
        double mx = -1e9;
        for( double t = 0.0; t < 1.5; t += dt ) {
            const double acc[3] = { trunkPhiDD( t, ramp, amp ), 0.0, 0.0 };
            twBodyJointStep( j, st, acc, nullptr, dt );
            mx = std::max( mx, st.angle[(int) twBodyAxis::Flex] );
        }
        std::printf( "  overdamped (zeta=1.4): max forward %.6f deg\n", mx * 180.0 / kPi );
        check( mx < fwdSecond * 0.5, "an OVERDAMPED joint overshoots far less" );
    }

    // --- 9. INFORMATIONAL: what stiffness and damping actually imply -------
    // NOT assertions. The 45 deg overshoot section 3 reports is a consequence
    // of stiffnessScale = 1.0, which is GRAVITY ALONE -- a deliberate floor,
    // not a realistic neck. Real passive joint stiffness is one of the numbers
    // proposal 44 section 3 flags VERIFY and this environment could not source.
    // Recorded here so whoever wires a joint to the display picks from data
    // instead of guessing, and so the choice is visible when AC2.1 closes.
    {
        std::printf( "\n  PASSIVE stiffness (x the head's own gravity moment) and damping\n"
                     "  -> natural Hz, forward overshoot for a 25 deg torso stop.\n"
                     "  Note ks = 1.0 is the STABILITY THRESHOLD for this inverted\n"
                     "  joint, not a soft setting: there, passive tissue exactly\n"
                     "  cancels gravity and the neck holds nothing up.\n" );
        for( double ks : { 1.5, 2.0, 5.0, 10.0, 17.0 } ) {
            for( double z : { 0.2, 0.4, 0.7 } ) {
                twBodyJoint j = neckJoint( z );
                j.stiffnessScale = ks;
                twBodyJointState st;
                double mx = -1e9;
                for( double t = 0.0; t < 1.5; t += dt ) {
                    const double acc[3] = { trunkPhiDD( t, 0.25, 25.0 * kPi / 180.0 ), 0.0, 0.0 };
                    twBodyJointStep( j, st, acc, nullptr, dt );
                    mx = std::max( mx, st.angle[(int) twBodyAxis::Flex] );
                }
                std::printf( "    k x%-5.1f zeta %.1f -> %6.3f Hz  %8.2f deg\n",
                             ks, z, twBodyJointNaturalHz( j, twBodyAxis::Flex ),
                             mx * 180.0 / kPi );
            }
        }
        std::printf( "\n" );
    }

    if( g_fail == 0 ) std::printf( "body_joint_test: PASS\n" );
    else              std::printf( "body_joint_test: %d FAILURES\n", g_fail );
    return g_fail ? 1 : 0;
}
