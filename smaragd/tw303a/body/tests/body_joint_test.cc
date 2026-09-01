// Proposal 44 C4 (first half) -- a joint with inertia.
//
// THE CENTRAL TEST IS THE REQUESTER'S OWN, and it is section 3 below: move the
// torso forward, then STOP; the head must keep going and nod FORWARD, then ring
// down. Section 4 is its control -- the same input through the FIRST-ORDER lag
// the model shipped, which can never go forward at all. Those two together are
// the whole argument, stated as a pair so neither can pass by accident.
//
// SECTION 3a IS THE SECOND FALSIFIER, added after an adversarial review found
// the first build's equation missing a term that is O(100%) of the response.
// Gravity acts on a segment's ABSOLUTE angle, and the first build applied it to
// the joint's RELATIVE angle only -- so a freely hanging arm on a leaning trunk
// settled PARALLEL TO THE LEANING TRUNK instead of hanging plumb. One sentence,
// no constants, no tolerance argument, and it cannot be satisfied by tuning.
// It is written before the requester's own test in reading order for a reason:
// section 3's headline number is a CONSEQUENCE of the equation being right,
// and section 3a is the thing that says the equation is right.
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
static const int kFlex  = (int) twBodyAxis::Flex;

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
static double trunkPhiD( double t, double ramp, double amp )
{
    const double h = 1e-5;
    return ( trunkPhi( t + h, ramp, amp ) - trunkPhi( t - h, ramp, amp ) ) / ( 2.0 * h );
}
static double trunkPhiDD( double t, double ramp, double amp )
{
    const double h = 1e-5;
    return ( trunkPhi( t + h, ramp, amp ) - 2.0 * trunkPhi( t, ramp, amp )
             + trunkPhi( t - h, ramp, amp ) ) / ( h * h );
}

/** The full parent state for the flex axis at time t. All three fields, which
 * is the point of the struct: the first build passed only the acceleration. */
static twBodyParentMotion trunkAt( double t, double ramp, double amp )
{
    twBodyParentMotion p;
    p.angle [kFlex] = trunkPhi  ( t, ramp, amp );
    p.angVel[kFlex] = trunkPhiD ( t, ramp, amp );
    p.angAcc[kFlex] = trunkPhiDD( t, ramp, amp );
    return p;
}

/** A parent standing perfectly still. */
static twBodyParentMotion still() { return twBodyParentMotion(); }

int main()
{
    const double dt = 0.0002;

    // --- 1. the integrator CONSERVES, which is what catches an integrator bug
    {
        twBodyJoint j = neckJoint( 0.0 );        // no damping, no drive
        twBodyJointState st;
        st.angle[kFlex] = 0.2;
        const double e0 = twBodyJointEnergy( j, st, twBodyAxis::Flex );
        double eMin = e0, eMax = e0;
        for( long n = 0; n < (long) ( 60.0 / dt ); n++ ) {
            twBodyJointStep( j, st, still(), nullptr, dt );
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
        check( twBodyJointStiffness( weak, twBodyAxis::Flex ) < 0.0,
               "an inverted joint with passive stiffness below its gravity moment"
               " has NEGATIVE total stiffness" );
        check( twBodyJointNaturalHz( weak, twBodyAxis::Flex ) == 0.0,
               "and reports no natural frequency rather than a fictitious one" );
        twBodyJoint arm;
        const twBodyMeasures mm;
        arm.segment = mm.compound( twBodySeg::UpperArm, 3 );
        arm.invertedPendulum = false;                  // an arm HANGS
        arm.stiffnessScale   = 0.0;                    // no passive tissue at all
        check( twBodyJointStiffness( arm, twBodyAxis::Flex ) > 0.0,
               "a HANGING segment is stable on gravity alone -- opposite sign" );
    }

    // --- 2a. AN UNSTABLE JOINT ACTUALLY FALLS -----------------------------
    // The first build folded k < 0 into "no stiffness", which ALSO discarded
    // the damping, so a head below its own stability threshold -- displaced and
    // then left completely alone -- sat exactly where it was put, forever. It
    // reported the instability (section 2) and did not exhibit it. The exact
    // step's hyperbolic branch is what makes the two agree.
    {
        twBodyJoint j = neckJoint( 0.30, 0.6 );        // below threshold
        twBodyJointState st;
        st.angle[kFlex] = 0.2;                         // displaced, no drive
        const double k = twBodyJointStiffness( j, twBodyAxis::Flex );
        const double I = twBodyJointInertia( j, twBodyAxis::Flex );
        const double lambda = std::sqrt( -k / I );     // the divergence rate
        for( long n = 0; n < (long) ( 1.0 / dt ); n++ )
            twBodyJointStep( j, st, still(), nullptr, dt );
        std::printf( "  unstable (ks=0.6): 0.2000 rad -> %.4f rad after 1 s"
                     " (undamped cosh rate lambda = %.2f /s)\n",
                     st.angle[kFlex], lambda );
        check( st.angle[kFlex] > 1.0,
               "an inverted joint below its threshold FALLS -- it does not sit still" );
        check( st.vel[kFlex] > 0.0, "and it is still accelerating away" );
    }

    // --- 3a. THE SECOND FALSIFIER: A HANGING ARM HANGS PLUMB --------------
    // Gravity acts on a segment's ABSOLUTE angle. Lean the trunk and hold it
    // there; a shoulder with no passive tissue at all must settle so the arm
    // points DOWN, i.e. at exactly minus the trunk's lean. Under the truncated
    // equation gravity saw only the relative angle, and the arm settled at 0 --
    // parallel to the leaning trunk, sticking out at 25 degrees from vertical
    // and perfectly happy there. No constant in this section, and no tuning
    // can produce -phi from an equation that has no phi in it.
    {
        const twBodyMeasures m;
        twBodyJoint arm;
        arm.segment          = m.compound( twBodySeg::UpperArm, 3 );
        arm.freeAxes         = twBodySpherical();
        arm.invertedPendulum = false;                  // it HANGS
        arm.stiffnessScale   = 0.0;                    // no passive tissue
        arm.dampingRatio     = 0.7;
        arm.parentLever      = 0.0;                    // isolate gravity alone

        const double lean = 25.0 * kPi / 180.0;
        twBodyParentMotion p;                          // leaning, and STILL
        p.angle[kFlex] = lean;

        twBodyJointState st;
        for( long n = 0; n < (long) ( 30.0 / dt ); n++ )
            twBodyJointStep( arm, st, p, nullptr, dt );
        const double rel = st.angle[kFlex];
        std::printf( "  trunk holds a %.1f deg lean: a free arm settles at"
                     " %+.4f deg relative -> %+.4f deg absolute\n",
                     lean * 180.0 / kPi, rel * 180.0 / kPi,
                     ( lean + rel ) * 180.0 / kPi );
        near( rel, -lean, 1e-4, "A FREE ARM HANGS PLUMB: relative angle = -lean" );
        near( lean + rel, 0.0, 1e-4, "i.e. its ABSOLUTE angle is vertical" );
        // And the equilibrium query agrees with where it actually went.
        near( twBodyJointEquilibrium( arm, twBodyAxis::Flex, p ), -lean, 1e-9,
              "and twBodyJointEquilibrium() says so without integrating" );
    }

    // --- 3. THE REQUESTER'S TEST ------------------------------------------
    // Move the torso forward, then stop. The head has momentum; it must keep
    // going forward, i.e. the relative angle must go POSITIVE, then ring down.
    //
    // WHAT IT RINGS DOWN *TO* IS NOT ZERO, and the first build asserted that it
    // was. With gravity acting on the absolute angle, an inverted head on a
    // leaning trunk DROOPS: it settles at a positive relative angle, and the
    // ringing is about THAT. Measuring the ring about zero is what made the
    // truncated equation look like it was behaving.
    double fwdSecond = 0.0, restRel = 0.0;
    {
        twBodyJoint j = neckJoint( 0.30 );
        twBodyJointState st;
        const double ramp = 0.25, amp = 25.0 * kPi / 180.0;
        std::vector<double> rel;
        for( double t = 0.0; t < 6.0; t += dt ) {
            twBodyJointStep( j, st, trunkAt( t, ramp, amp ), nullptr, dt );
            rel.push_back( st.angle[kFlex] );
        }
        double mx = 0.0; for( double v : rel ) mx = std::max( mx, v );
        fwdSecond = mx;
        restRel   = rel.back();

        // The equilibrium the trunk's HELD lean implies, once it has stopped.
        twBodyParentMotion held; held.angle[kFlex] = amp;
        const double eq = twBodyJointEquilibrium( j, twBodyAxis::Flex, held );

        int crossings = 0;
        for( size_t i = 1; i < rel.size(); i++ )
            if( ( rel[i-1] - eq ) * ( rel[i] - eq ) < 0.0 ) crossings++;
        std::printf( "  torso flexes 25 deg then STOPS: head forward overshoot"
                     " %.3f deg, settles at %+.3f deg (equilibrium %+.3f),"
                     " %d crossings of it\n",
                     mx * 180.0 / kPi, restRel * 180.0 / kPi,
                     eq * 180.0 / kPi, crossings );
        check( mx > 0.01, "THE HEAD NODS FORWARD after the torso stops" );
        check( mx > eq + 0.01,
               "and it OVERSHOOTS its own resting droop -- momentum, not a lean" );
        check( crossings >= 2,
               "and it RINGS about that equilibrium -- an overshoot, not a drift" );
        near( restRel, eq, 1e-3,
              "and it comes to rest exactly where the equilibrium says" );
        check( eq > 0.0,
               "which for an INVERTED head on a leaning trunk is a forward DROOP,"
               " not trunk-aligned" );
    }

    // --- 3b. POSTURAL TONE: THE OTHER HALF OF tau_muscle ------------------
    // Proposal 44 section 8 question 3 was settled as TORQUE drive, so nothing
    // in the musical signal knows which way is down -- and an inverted segment
    // needs a standing torque simply to stay up. That hold is SPLIT OUT as
    // posturalGain rather than left to ride inside the musical torque as a DC
    // offset, and it is DERIVED (whatever cancels m*g*d) rather than chosen.
    //
    // The gate is a PAIR, because either half alone is satisfiable by a
    // constant. At gain 0 a free arm HANGS PLUMB (section 3a); at gain 1 the
    // same arm STAYS WHERE IT IS PUT, which is what holding your arm out IS.
    // No model that ignores the gain can produce both.
    {
        const twBodyMeasures m;
        twBodyJoint arm;
        arm.segment          = m.compound( twBodySeg::UpperArm, 3 );
        arm.freeAxes         = twBodySpherical();
        arm.invertedPendulum = false;
        arm.stiffnessScale   = 0.0;              // no passive tissue at all
        arm.dampingRatio     = 0.7;
        arm.parentLever      = 0.0;
        arm.posturalGain     = 1.0;              // ...but muscle IS holding it

        const double lean = 25.0 * kPi / 180.0;
        twBodyParentMotion p; p.angle[kFlex] = lean;

        twBodyJointState st;
        st.angle[kFlex] = 0.6;                   // put it somewhere deliberate
        for( long n = 0; n < (long) ( 30.0 / dt ); n++ )
            twBodyJointStep( arm, st, p, nullptr, dt );
        std::printf( "  fully toned arm, placed at %.4f rad on a %.1f deg lean:"
                     " %.4f rad after 30 s (postural torque %.3f N m)\n",
                     0.6, lean * 180.0 / kPi, st.angle[kFlex],
                     twBodyPosturalTorque( arm, twBodyAxis::Flex, p, st ) );
        near( st.angle[kFlex], 0.6, 1e-9,
              "AT FULL POSTURAL TONE A FREE JOINT STAYS WHERE IT IS PUT" );
        near( twBodyJointStiffness( arm, twBodyAxis::Flex ), 0.0, 1e-12,
              "-- gravity is gone from the stiffness entirely" );
        // ...and the same joint at gain 0 is section 3a's plumb arm. Half a
        // gain gives half the pull, exactly: the compensation is linear.
        for( double gain : { 0.0, 0.25, 0.5, 0.75 } ) {
            twBodyJoint a2 = arm; a2.posturalGain = gain;
            twBodyJointState s2;
            for( long n = 0; n < (long) ( 40.0 / dt ); n++ )
                twBodyJointStep( a2, s2, p, nullptr, dt );
            near( s2.angle[kFlex], -lean, 1e-4,
                  "a PARTLY toned free arm still settles plumb -- scaling both"
                  " gravity terms alike moves the equilibrium not at all" );
        }

        // THE HOLD COSTS MUSCLE, and C4b's objective has to see it. An effort
        // term counting only the active torque would report a body straining
        // against a beat as cheaper than one merely standing leant over.
        twBodyJoint head = neckJoint(); head.posturalGain = 1.0;
        twBodyJointState hs;
        twBodyParentMotion hp; hp.angle[kFlex] = lean;
        const double tp = twBodyPosturalTorque( head, twBodyAxis::Flex, hp, hs );
        std::printf( "  a fully toned head on a %.1f deg lean holds %.4f N m\n",
                     lean * 180.0 / kPi, tp );
        check( std::fabs( tp ) > 1e-3, "holding a head up on a lean COSTS torque" );
        twBodyJoint limp = neckJoint();          // gain 0
        twBodyJointState ls;
        near( twBodyPosturalTorque( limp, twBodyAxis::Flex, hp, ls ), 0.0, 0.0,
              "and an unactivated joint spends none -- it droops instead" );
        // Proportional to how much is being held, and to how far over it is.
        twBodyJoint half = neckJoint(); half.posturalGain = 0.5;
        near( twBodyPosturalTorque( half, twBodyAxis::Flex, hp, hs ), tp * 0.5,
              1e-12, "half the tone is exactly half the torque" );
        twBodyParentMotion up;                   // upright: nothing to hold
        near( twBodyPosturalTorque( head, twBodyAxis::Flex, up, hs ), 0.0, 0.0,
              "and an upright segment costs nothing to hold" );
    }

    // --- 3c. THE STABILITY THRESHOLD IS A CLAIM ABOUT AN UNACTIVATED JOINT --
    // ks > 1 says an inverted segment cannot hold itself up on LIGAMENT alone.
    // A live person's head is held up by MUSCLE, so under postural tone the
    // threshold does not apply -- and a joint that section 2a watched fall over
    // is perfectly stable once it is being held. Stating this as a test is what
    // stops the threshold being read as a claim about people.
    {
        twBodyJoint weak = neckJoint( 0.30, 0.6 );        // below threshold
        check( twBodyJointStiffness( weak, twBodyAxis::Flex ) < 0.0,
               "UNACTIVATED, a ks = 0.6 inverted joint has negative stiffness" );
        twBodyJoint toned = weak; toned.posturalGain = 1.0;
        check( twBodyJointStiffness( toned, twBodyAxis::Flex ) > 0.0,
               "TONED, the same joint is stable -- the threshold is about"
               " ligament, not about a person" );
        twBodyJointState st;
        st.angle[kFlex] = 0.2;
        for( long n = 0; n < (long) ( 1.0 / dt ); n++ )
            twBodyJointStep( toned, st, still(), nullptr, dt );
        std::printf( "  the SAME joint section 2a watched fall: toned, 0.2000"
                     " -> %.4f rad after 1 s\n", st.angle[kFlex] );
        check( std::fabs( st.angle[kFlex] ) < 0.2,
               "and it returns toward upright instead of falling over" );
    }

    // --- 3d. THE LEVER COUPLING IS SIGNED, AND A HANGING SEGMENT IS THE
    // --- CASE THAT SHOWS IT -----------------------------------------------
    // Found by DERIVING the coupled chain (C8) rather than by reading this
    // file: writing the two-segment Lagrangian in ABSOLUTE angles gives an
    // off-diagonal mass term  m * (sigma_p * L) * (sigma_c * d), and the sign
    // of a HANGING child is NEGATIVE. `forcingTorque` carried `-(I + m*d*L)`
    // unsigned, which is the INVERTED answer applied to every joint.
    //
    // The falsifier needs no constant and no tolerance argument. Strip the
    // joint to pure kinematics -- a POINT MASS (inertiaProx = m*d^2), no
    // passive stiffness, no damping, gravity fully compensated -- and the
    // answer is arithmetic:
    //
    //     psi_child'' = -sigma_p*sigma_c * (L/d) * phi''
    //
    // i.e. a mass that is simply left behind while its joint is carried away.
    // In the joint's own RELATIVE angle that is
    //
    //     theta'' = -( 1 + sigma * L/d ) * phi''      (sigma_p = +1 here)
    //
    // At L/d = 2 the inverted child reads -3*phi'' and the hanging one +1*phi''
    // -- opposite in SIGN and three times apart in MAGNITUDE, so no tuning of
    // any other term can stand in for the sign.
    {
        const double m = 4.0, d = 0.25, L = 0.5;   // L/d = 2 exactly
        const double A = 3.0;                      // rad/s^2, the parent's step
        for( int inv = 0; inv < 2; inv++ ) {
            twBodyJoint j;
            j.segment.mass        = m;
            j.segment.length      = 2.0 * d;
            j.segment.comFromProx = d;
            j.segment.radiusGyrCom = 0.0;          // A POINT MASS: I_com = 0,
            j.segment.inertiaProx  = m * d * d;    // so I about the joint is m*d^2
            j.segment.radiusGyrLong = 0.0;
            j.segment.inertiaLong   = 0.0;
            j.freeAxes         = twBodySpherical();
            j.invertedPendulum = ( inv != 0 );
            j.stiffnessScale   = 0.0;              // no passive tissue
            j.dampingRatio     = 0.0;              // no damping
            j.posturalGain     = 1.0;              // gravity fully cancelled
            j.parentLever      = L;

            twBodyParentMotion p;
            p.angAcc[kFlex] = A;
            twBodyJointState st;
            const double dt = 1e-4;
            twBodyJointStep( j, st, p, nullptr, dt );
            const double got  = st.vel[kFlex] / dt;             // theta''
            const double sig  = j.invertedPendulum ? +1.0 : -1.0;
            const double want = -( 1.0 + sig * ( L / d ) ) * A;
            std::printf( "  point mass, %s, L/d = 2: theta'' = %+.4f"
                         " (kinematic closed form %+.4f)\n",
                         j.invertedPendulum ? "INVERTED" : "HANGING ", got, want );
            near( got, want, 1e-6 * std::fabs( want ) + 1e-9,
                  j.invertedPendulum
                      ? "an inverted point mass is left behind by its joint"
                      : "a HANGING point mass is left behind by its joint" );
        }
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
    // MEASURED FROM THE EQUILIBRIUM, not from zero. A driven joint's resting
    // place is set by where its parent is holding, and "has the ringing died"
    // is a question about the distance from THAT. The first build measured
    // from zero, which is only the same question when the equilibrium happens
    // to be zero -- i.e. only under the equation that had no parent angle in it.
    {
        twBodyJoint j = neckJoint( 0.30 );
        twBodyJointState st;
        const double ramp = 0.25, amp = 25.0 * kPi / 180.0;
        twBodyParentMotion held; held.angle[kFlex] = amp;
        const double eq = twBodyJointEquilibrium( j, twBodyAxis::Flex, held );

        double peak = 0.0; double t = 0.0;
        for( ; t < 0.6; t += dt ) {
            twBodyJointStep( j, st, trunkAt( t, ramp, amp ), nullptr, dt );
            peak = std::max( peak, std::fabs( st.angle[kFlex] - eq ) );
        }
        const double cycles = 6.0 / std::max( 1e-6, twBodyJointNaturalHz( j, twBodyAxis::Flex ) );
        for( double u = 0.0; u < cycles; u += dt )
            twBodyJointStep( j, st, held, nullptr, dt );
        const double left = std::fabs( st.angle[kFlex] - eq ) / peak;
        std::printf( "  arrest: %.4f%% of peak DEVIATION left after 6 natural"
                     " periods (equilibrium %+.3f deg)\n",
                     100.0 * left, eq * 180.0 / kPi );
        check( left < 0.05, "a damped joint ARRESTS -- under 5%% of peak after 6 periods" );
    }

    // --- 6. THE DOF CONSTRAINT: a hinge is a hinge ------------------------
    // A constrained axis does not move, whatever drives it. Not "very stiff" --
    // zero. This is the rule that makes a knee different from a tightened ball.
    {
        twBodyJoint j = neckJoint();
        j.freeAxes = twBodyHinge( twBodyAxis::Flex );      // sagittal only
        twBodyJointState st;
        twBodyParentMotion p;
        for( int i = 0; i < 3; i++ ) p.angAcc[i] = 40.0;   // drive ALL three hard
        for( long n = 0; n < 5000; n++ ) twBodyJointStep( j, st, p, nullptr, dt );
        check( std::fabs( st.angle[kFlex] ) > 1e-4,
               "the FREE axis of a hinge moves" );
        near( st.angle[(int) twBodyAxis::Lateral], 0.0, 0.0, "a hinge does not move laterally" );
        near( st.angle[(int) twBodyAxis::Axial],   0.0, 0.0, "a hinge does not twist" );
        near( st.vel[(int) twBodyAxis::Lateral],   0.0, 0.0, "nor accumulate lateral velocity" );
        check( twBodyJointNaturalHz( j, twBodyAxis::Axial ) == 0.0,
               "a constrained axis reports no natural frequency" );
    }

    // --- 6a. THE LONG AXIS IS NOT THE SAGITTAL PLANE ----------------------
    // Twisting a segment neither raises nor lowers its centre of mass, so
    // gravity exerts NO moment about its long axis -- no +/- m*g*d term, no
    // stability threshold, no crossover -- and the inertia it turns against is
    // its own longitudinal one, with no parallel-axis term because the CoM sits
    // ON that axis. The first build gave twist the sagittal plane's inertia,
    // stiffness and drive wholesale, which made a head as slow to turn as it is
    // to nod. Every claim here is a RELATION, so none of it rests on the girth
    // column's provisional value.
    {
        const twBodyJoint j = neckJoint();             // inverted, ks = 2
        const double iFlex = twBodyJointInertia( j, twBodyAxis::Flex );
        const double iAx   = twBodyJointInertia( j, twBodyAxis::Axial );
        const double kFl   = twBodyJointStiffness( j, twBodyAxis::Flex );
        const double kAx   = twBodyJointStiffness( j, twBodyAxis::Axial );
        std::printf( "  axes: I_flex %.5f vs I_axial %.5f kg m^2;"
                     " k_flex %.3f vs k_axial %.3f N m/rad;"
                     " %.3f Hz vs %.3f Hz\n", iFlex, iAx, kFl, kAx,
                     twBodyJointNaturalHz( j, twBodyAxis::Flex ),
                     twBodyJointNaturalHz( j, twBodyAxis::Axial ) );
        check( iAx < iFlex * 0.25,
               "twist inertia is far below flexion inertia -- no parallel-axis term" );
        // ks = 2 inverted: flexion keeps (2-1) x m*g*d, twist keeps the full 2x.
        near( kAx / kFl, 2.0, 1e-9,
              "twist carries NO gravity term: k_axial / k_flex = ks / (ks-1)" );
        check( twBodyJointNaturalHz( j, twBodyAxis::Axial )
               > twBodyJointNaturalHz( j, twBodyAxis::Flex ),
               "so a segment twists FASTER than it flexes" );
        // The stability threshold is a flexion-plane fact only.
        twBodyJoint weak = neckJoint( 0.3, 0.6 );
        check( twBodyJointStiffness( weak, twBodyAxis::Flex ) < 0.0
               && twBodyJointStiffness( weak, twBodyAxis::Axial ) > 0.0,
               "a joint unstable in FLEXION is still perfectly stable in TWIST" );
        // And the parent's lever does not reach the long axis: this joint sits
        // ON it, so a parent twisting produces no linear acceleration here.
        twBodyJoint jj = neckJoint();
        twBodyParentMotion p; p.angVel[(int) twBodyAxis::Axial] = 12.0;
        near( twBodyJointStiffnessAt( jj, twBodyAxis::Axial, p ),
              twBodyJointStiffness( jj, twBodyAxis::Axial ), 1e-12,
              "and a fast parent twist adds NO centripetal stiffening about it" );
    }

    // --- 6b. CENTRIPETAL STIFFENING GROWS AS THE SQUARE OF THE RATE -------
    // A segment hanging off a rotating parent is flung outward, and the
    // restoring part of that scales with the parent's angular rate squared. It
    // is negligible at a slow sway and DOMINANT at the rates this feature is
    // about, which is why it cannot be left out of a model that claims to work
    // at 2-4 Hz. It always stiffens, never softens.
    {
        const twBodyJoint j = neckJoint();
        const double k0 = twBodyJointStiffness( j, twBodyAxis::Flex );
        std::printf( "  centripetal stiffening of the neck at a 25 deg sway,"
                     " against k = %.3f N m/rad:\n", k0 );
        double prev = 0.0;
        for( double hz : { 0.5, 1.0, 2.0, 4.0 } ) {
            const double amp = 25.0 * kPi / 180.0;
            const double w   = 2.0 * kPi * hz * amp;      // peak dphi/dt
            twBodyParentMotion p; p.angVel[kFlex] = w;
            const double add = twBodyJointStiffnessAt( j, twBodyAxis::Flex, p ) - k0;
            std::printf( "    %.1f Hz -> +%.3f N m/rad (%.0f%% of k)\n",
                         hz, add, 100.0 * add / k0 );
            check( add > 0.0, "centripetal stiffening always ADDS" );
            if( prev > 0.0 )
                near( add / prev, 4.0, 1e-6,
                      "and quadruples for each doubling of the rate" );
            prev = add;
        }
        check( prev > k0, "at 4 Hz it EXCEEDS the joint's own stiffness" );
    }

    // --- 6c. DAMPING DOES NOT MOVE WHEN STIFFNESS DOES --------------------
    // Damping is a property of tissue. Parameterising it as a RATIO ties the
    // absolute coefficient to sqrt(k), so a joint whose stiffness is modulated
    // -- centripetally here, by co-contraction once C4b lands -- would silently
    // modulate its own damping too, and c would VANISH exactly at the stability
    // threshold, where an inverted joint needs it most.
    {
        const twBodyJoint j = neckJoint();
        const double c0 = twBodyJointDamping( j, twBodyAxis::Flex );
        twBodyParentMotion fast; fast.angVel[kFlex] = 8.0;
        near( twBodyJointDamping( j, twBodyAxis::Flex ), c0, 0.0,
              "damping is unchanged by centripetal stiffening" );
        check( c0 > 0.0, "and is positive" );
        // At the threshold the TOTAL stiffness is zero; the damping is not.
        twBodyJoint edge = neckJoint( 0.30, 1.0 );
        near( twBodyJointStiffness( edge, twBodyAxis::Flex ), 0.0, 1e-9,
              "ks = 1 is exactly the stability threshold for an inverted joint" );
        check( twBodyJointDamping( edge, twBodyAxis::Flex ) > 0.0,
               "AND ITS DAMPING DOES NOT VANISH THERE" );
        // Flipping the inverted flag changes the stiffness, never the damping.
        twBodyJoint hang = neckJoint(); hang.invertedPendulum = false;
        check( twBodyJointStiffness( hang, twBodyAxis::Flex )
               != twBodyJointStiffness( j, twBodyAxis::Flex ),
               "the inverted flag moves the stiffness" );
        near( twBodyJointDamping( hang, twBodyAxis::Flex ), c0, 1e-12,
              "and leaves the damping exactly where it was" );
    }

    // --- 7. RANGE OF MOTION: the stop is dissipative ----------------------
    {
        twBodyJoint j = neckJoint( 0.05 );
        j.limitRad[kFlex] = 15.0 * kPi / 180.0;
        twBodyJointState st;
        twBodyParentMotion p; p.angAcc[kFlex] = 60.0;
        double worst = 0.0;
        for( long n = 0; n < 20000; n++ ) {
            twBodyJointStep( j, st, p, nullptr, dt );
            worst = std::max( worst, std::fabs( st.angle[kFlex] ) );
        }
        std::printf( "  range of motion: worst |angle| %.4f deg against a 15.0 limit,"
                     " %u steps spent at the stop\n", worst * 180.0 / kPi, st.limitHits );
        check( worst <= 15.0 * kPi / 180.0 + 1e-12, "the joint never exceeds its limit" );
        check( st.limitHits > 0, "and the stop was actually reached" );
    }

    // --- 8. an OVERDAMPED joint overshoots FAR LESS -----------------------
    // The damping ratio is the knob that decides how big a nod is, so both
    // sides of it are asserted rather than one. Note the claim is comparative:
    // an overdamped joint CANNOT overshoot in free decay, but under a bipolar
    // drive -- accelerate, then decelerate, which is exactly the torso stop --
    // it crosses its equilibrium perfectly happily. The header used to state
    // the absolute, and this test's own numbers contradicted it.
    {
        twBodyJoint j = neckJoint( 1.4 );
        twBodyJointState st;
        const double ramp = 0.25, amp = 25.0 * kPi / 180.0;
        double mx = -1e9;
        for( double t = 0.0; t < 1.5; t += dt ) {
            twBodyJointStep( j, st, trunkAt( t, ramp, amp ), nullptr, dt );
            mx = std::max( mx, st.angle[kFlex] );
        }
        std::printf( "  overdamped (zeta=1.4): max forward %.6f deg\n", mx * 180.0 / kPi );
        check( mx < fwdSecond * 0.5, "an OVERDAMPED joint overshoots far less" );
    }

    // --- 10. THE BRACED BOUNCE: can the plant do what the requester DESCRIBES?
    // Asked before C4b builds a controller on top, because a controller that
    // optimises toward a motion the plant cannot produce is built on sand.
    //
    // The description (requester, 2026-08-30, on the Enter Sandman build-up):
    // raise postural tone between trunk and head, drive the head down HARD, and
    // have it "bounce up again like a ball thrown forcefully on the floor", to
    // then push it down again -- the bounce being what enables a HARSHER next
    // strike.
    //
    // **THE FIRST READING OF THAT WAS WRONG AND IS RECORDED SO IT IS NOT
    // RE-TRIED.** It was written as "a braced head bounces, a slack one does
    // not -- its own weight eats the return", and the model says otherwise:
    // an underdamped joint RINGS whether or not it is toned, so the slack head
    // comes back past its resting place too. What actually separates them is
    // WHERE THEY REST and HOW FAST THEY TURN AROUND, and those are the two
    // things "hard and sudden" names.
    {
        const double lean  = 20.0 * kPi / 180.0;
        const double pulse = 0.06;                 // s, the strike
        const double tau   = 6.0;                  // N m, downward (+ = forward)
        twBodyParentMotion held; held.angle[kFlex] = lean;

        struct Result { double rest, down, backMs; };
        auto strike = [&]( double ks, double gain ) {
            twBodyJoint j = neckJoint( 0.15, ks );
            j.posturalGain = gain;
            twBodyJointState st;
            // Settle where the body is ALREADY holding: the strike is a
            // departure from that, not from an arbitrary zero.
            for( long n = 0; n < (long) ( 20.0 / dt ); n++ )
                twBodyJointStep( j, st, held, nullptr, dt );
            const double start = st.angle[kFlex];
            double down = start, tDown = -1.0, backMs = -1.0;
            for( double t = 0.0; t < 3.0; t += dt ) {
                const double act[3] = { t < pulse ? tau : 0.0, 0.0, 0.0 };
                twBodyJointStep( j, st, held, act, dt );
                if( st.angle[kFlex] > down ) { down = st.angle[kFlex]; tDown = t; }
                else if( tDown >= 0.0 && backMs < 0.0
                         && st.angle[kFlex] <= start )
                    backMs = ( t - tDown ) * 1000.0;
            }
            return Result{ start, down - start, backMs };
        };

        const Result braced = strike( 8.0, 1.0 );   // toned, stiff
        const Result slack  = strike( 1.5, 0.0 );   // untoned, floppy
        std::printf( "\n  THE BRACED BOUNCE (same %.1f N m strike for %.0f ms,"
                     " trunk held at %.0f deg):\n", tau, pulse * 1000.0,
                     lean * 180.0 / kPi );
        std::printf( "    braced (ks 8, tone 1.0): rests %+6.2f deg, travels"
                     " %5.2f deg down, %5.0f ms back to rest\n",
                     braced.rest * 180.0 / kPi, braced.down * 180.0 / kPi,
                     braced.backMs );
        std::printf( "    slack  (ks 1.5, tone 0 ): rests %+6.2f deg, travels"
                     " %5.2f deg down, %5.0f ms back to rest\n",
                     slack.rest * 180.0 / kPi, slack.down * 180.0 / kPi,
                     slack.backMs );

        // WHERE IT RESTS. A toned head sits trunk-aligned however far the trunk
        // leans; an untoned one is already hanging 40 degrees below it before
        // the strike lands -- half its range spent on nothing.
        near( braced.rest, 0.0, 1e-9,
              "a toned head rests TRUNK-ALIGNED whatever the lean" );
        check( slack.rest > lean,
               "an untoned head hangs BELOW the trunk before the strike lands" );

        // HOW FAST IT TURNS AROUND. This is what "hard and sudden" means, and
        // it is a quarter period, so it is the resonance shift of section 11
        // showing up in the time domain.
        check( braced.backMs > 0.0 && slack.backMs > 0.0,
               "BOTH ring -- an underdamped joint returns whether toned or not,"
               " which is the correction to this section's first reading" );
        check( braced.backMs * 3.0 < slack.backMs,
               "but the BRACED turnaround is at least 3x faster: that, not the"
               " existence of a return, is what makes the bounce SUDDEN" );
        check( braced.down < slack.down,
               "and the braced head travels LESS far -- the bounce is not the"
               " same motion done harder" );
    }

    // --- 11. WHY THE BRACE HELPS: the joint resonates, and TONE MOVES IT ----
    // The requester's account of the bounce -- an elastic return that enables a
    // harsher NEXT strike -- is the single-joint statement of resonance, and it
    // is the premise C4b optimises against. Measured, not assumed, and against
    // CLOSED FORMS so nothing here rests on a provisional constant.
    //
    // The gate is the resonant amplification over the STATIC deflection, which
    // is exactly 1/(2*zeta_eff). Asserting instead that the peak beats the
    // response at half the rate does NOT work and was tried: a soft joint's
    // quasi-static response at f_n/2 is large simply because it is soft, so the
    // ratio there is 1.5x and says nothing about resonance.
    {
        auto amplitudeAt = [&]( const twBodyJoint &j, double drive, double hz ) {
            twBodyJointState st;
            const double w = 2.0 * kPi * hz;
            double lo = 1e30, hi = -1e30;
            for( double t = 0.0; t < 14.0; t += dt ) {
                const double act[3] = { drive * std::sin( w * t ), 0.0, 0.0 };
                twBodyJointStep( j, st, still(), act, dt );
                if( t >= 10.0 ) {                       // steady state only
                    lo = std::min( lo, st.angle[kFlex] );
                    hi = std::max( hi, st.angle[kFlex] );
                }
            }
            return 0.5 * ( hi - lo );
        };

        std::printf( "\n  RESONANCE, and that TONE MOVES IT (2 N m sinusoid,"
                     " steady state):\n" );
        struct Cfg { double ks, gain; const char *name; };
        const Cfg cfgs[] = { { 2.0, 0.0, "ks 2, untoned" },
                             { 2.0, 1.0, "ks 2, toned  " },
                             { 8.0, 1.0, "ks 8, toned  " } };
        const double drive = 2.0;
        for( const Cfg &cf : cfgs ) {
            twBodyJoint j = neckJoint( 0.15, cf.ks );
            j.posturalGain = cf.gain;
            const double fn   = twBodyJointNaturalHz( j, twBodyAxis::Flex );
            const double k    = twBodyJointStiffness( j, twBodyAxis::Flex );
            const double zEff = twBodyJointDampingRatioAt( j, twBodyAxis::Flex,
                                                           still() );
            const double statc = drive / k;             // DC deflection
            const double aOn   = amplitudeAt( j, drive, fn );
            const double Q     = 1.0 / ( 2.0 * zEff );
            std::printf( "    %s  f_n %5.3f Hz  zeta_eff %.4f  static %5.2f deg"
                         " -> %6.2f deg at f_n  (Q %.3f, closed form %.3f)\n",
                         cf.name, fn, zEff, statc * 180.0 / kPi,
                         aOn * 180.0 / kPi, aOn / statc, Q );
            near( aOn / statc, Q, 0.02,
                  "the resonant amplification is exactly 1/(2*zeta_eff) over"
                  " the static deflection" );
            check( Q > 1.5, "and it IS an amplification -- there is a bounce"
                            " to be had at this tone" );
        }

        // A SOFTER JOINT IS EFFECTIVELY MORE DAMPED, because c is absolute and
        // does not follow sqrt(k) (invariant 5). So stiffening does TWO things:
        // it moves the resonance up AND sharpens it. Both help the requester's
        // bounce, and C4b's optimiser has to read zeta_eff rather than the
        // stored ratio.
        {
            twBodyJoint soft = neckJoint( 0.15, 2.0 ); soft.posturalGain = 0.0;
            twBodyJoint hard = neckJoint( 0.15, 8.0 ); hard.posturalGain = 1.0;
            const double zs = twBodyJointDampingRatioAt( soft, twBodyAxis::Flex, still() );
            const double zh = twBodyJointDampingRatioAt( hard, twBodyAxis::Flex, still() );
            check( zs > 0.15 && zh < zs,
                   "a SOFTER joint is effectively more damped -- stiffening both"
                   " raises the resonance and sharpens it" );
            near( zs, 0.15 * std::sqrt( ( 2.0 + 1.0 ) / ( 2.0 - 1.0 ) ), 1e-9,
                  "zeta_eff is the closed form dampingRatio*sqrt(kRef/k)" );
        }

        // THE C4b PREMISE, stated as the relation it is: tone and stiffness are
        // what a body has to move its own resonance WITH. Without a range to
        // tune across, "the body tunes toward the beat" is not a thing a body
        // could do.
        twBodyJoint a = neckJoint( 0.15, 2.0 ); a.posturalGain = 0.0;
        twBodyJoint b = neckJoint( 0.15, 2.0 ); b.posturalGain = 1.0;
        twBodyJoint c = neckJoint( 0.15, 8.0 ); c.posturalGain = 1.0;
        const double fa = twBodyJointNaturalHz( a, twBodyAxis::Flex );
        const double fb = twBodyJointNaturalHz( b, twBodyAxis::Flex );
        const double fc = twBodyJointNaturalHz( c, twBodyAxis::Flex );
        std::printf( "    -> tone alone moves f_n %.3f -> %.3f Hz (%.2fx);"
                     " with co-contraction, to %.3f Hz (%.2fx)\n",
                     fa, fb, fb / fa, fc, fc / fa );
        check( fb > fa * 1.3,
               "POSTURAL TONE ALONE RAISES THE RESONANCE -- for an INVERTED"
               " joint, cancelling gravity IS stiffening it" );
        near( fb / fa, std::sqrt( 2.0 ), 1e-9,
              "and by the closed form sqrt(ks/(ks-1)), here exactly sqrt(2)" );
        check( fc > fb * 1.5,
               "co-contraction raises it further, so a body has a real RANGE to"
               " tune across. That range is what C4b optimises in" );
    }

    // --- 9. INFORMATIONAL: what stiffness and damping actually imply -------
    // NOT assertions. Two numbers per row, because with gravity acting on the
    // absolute angle they are different questions: the joint settles at a
    // DROOP set by the trunk's held lean, and the overshoot is measured on top
    // of that. A soft neck droops a long way and rings a long way; a stiff one
    // does neither. Real passive joint stiffness is one of the numbers proposal
    // 44 section 3 flags VERIFY and this environment could not source.
    {
        const double amp = 25.0 * kPi / 180.0;
        std::printf( "\n  PASSIVE stiffness (x the head's own gravity moment) and damping\n"
                     "  -> natural Hz, resting DROOP, and peak forward overshoot for a\n"
                     "  25 deg torso stop. Note ks = 1.0 is the STABILITY THRESHOLD for\n"
                     "  this inverted joint, not a soft setting: there, passive tissue\n"
                     "  exactly cancels gravity and the neck holds nothing up.\n" );
        for( double ks : { 1.5, 2.0, 5.0, 10.0, 17.0 } ) {
            for( double z : { 0.2, 0.4, 0.7 } ) {
                twBodyJoint j = neckJoint( z );
                j.stiffnessScale = ks;
                twBodyJointState st;
                double mx = -1e9;
                for( double t = 0.0; t < 6.0; t += dt ) {
                    twBodyJointStep( j, st, trunkAt( t, 0.25, amp ), nullptr, dt );
                    mx = std::max( mx, st.angle[kFlex] );
                }
                twBodyParentMotion held; held.angle[kFlex] = amp;
                const double eq = twBodyJointEquilibrium( j, twBodyAxis::Flex, held );
                std::printf( "    k x%-5.1f zeta %.1f -> %6.3f Hz  droop %6.2f deg"
                             "  peak %6.2f deg (+%.2f over droop)\n",
                             ks, z, twBodyJointNaturalHz( j, twBodyAxis::Flex ),
                             eq * 180.0 / kPi, mx * 180.0 / kPi,
                             ( mx - eq ) * 180.0 / kPi );
            }
        }
        std::printf( "\n" );
    }

    if( g_fail == 0 ) std::printf( "body_joint_test: PASS\n" );
    else              std::printf( "body_joint_test: %d FAILURES\n", g_fail );
    return g_fail ? 1 : 0;
}
