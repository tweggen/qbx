// Proposal 44 C8 -- THE INWARD PASS.
//
// THE CENTRAL TEST IS SECTION 1, and it is a reduction rather than a
// behaviour: a two-joint chain whose ROOT IS PRESCRIBED must reproduce
// `twBodyJointStep`, in every damping regime and on both sides of the
// inverted/hanging split. That is what makes every OTHER number here
// attributable: with the reduction pinned, the only difference between the
// one-way model and this one is the reaction, so a measured difference cannot
// be an integrator change, a coefficient change or a sign convention drifting
// between two implementations.
//
// IT TAKES FOUR CLAIMS AND NOT ONE, and finding that out was the substance of
// the section. The two models do not carry the same quantity across a step
// boundary and do not make the same assumption inside one, so an exact
// trajectory equality is not available and asking for it measures the
// DISCRETISATION rather than the equation. 1a is the machine-precision
// equality (static parent); 1b and 1d are the convergence orders; 1c is what
// says the residual belongs to the model being REPLACED. Read 1's own header
// before changing any tolerance here.
//
// Section 2 reads the mass matrix DIRECTLY rather than inferring it from an
// excursion. Proposal 44 has been caught twice by a bound that a wrong
// coefficient satisfies comfortably (the plant's torque scale, twice), so a
// closed form is asserted as a closed form.
//
// Section 3 is proposal 44's AC4.2 -- "an arm swing counter-rotates the trunk"
// -- which C4a's header stated in as many words that it did NOT meet. It is a
// two-by-two solve with an exact answer, and the one-way model's answer for the
// trunk is exactly zero, so the gate cannot pass by accident.
//
// Every constant here rests on tw_body's UNSOURCED anthropometry (AC2.1 is
// open), so magnitudes are asserted only where they are closed forms OF THOSE
// INPUTS -- never as biomechanical claims.

#include "tw/body/twbodychain.h"
#include "tw/body/twbodyjoint.h"
#include "tw/body/twbodymeasures.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

static int g_fail = 0;
static void check( bool ok, const char *what )
{ if( !ok ) { std::printf( "FAIL: %s\n", what ); g_fail++; } }
static void near( double got, double want, double tol, const char *what )
{ if( !( std::fabs( got - want ) <= tol ) ) {
    std::printf( "FAIL: %s -- got %.12f want %.12f (tol %.3g)\n",
                 what, got, want, tol );
    g_fail++; } }

static const double kPi = 3.14159265358979323846;
static const int kFlex = (int) twBodyAxis::Flex;

int main()
{
    // The DEFAULT body -- 75 kg, 1.75 m -- because section 2 reproduces a
    // figure the execution plan computed over exactly that one.
    const twBodyMeasures body;

    // --- 1. THE REDUCTION, in three separate claims -----------------------
    //
    // A single "the chain equals the single joint" gate is NOT available, and
    // finding out why was the substance of this section. The two models do not
    // carry the same quantity across a step boundary and do not make the same
    // assumption inside one: `twBodyJointStep` integrates the RELATIVE angle
    // with the parent's angle and rate FROZEN, so it is first-order accurate
    // under an accelerating parent; the chain integrates the ABSOLUTE angle and
    // carries the parent's own quadratic motion exactly. Neither is a bug.
    // So the reduction is stated as what it actually is:
    //
    //   1a  with a STATIC parent the two are IDENTICAL to machine precision --
    //       every coefficient, both gravity terms, the postural gain, the
    //       spring and damper assembly, and the integrator in all four regimes;
    //   1b  with a MOVING parent, from a matched state, they agree to O(dt^3);
    //   1c  and the CHAIN is the exact one -- one step of dt equals two of
    //       dt/2 to machine precision, where twBodyJointStep's does not;
    //   1d  so free-running, the trajectories converge FIRST order.
    //
    // Only 1a can be a machine-precision equality, and it is the one that pins
    // the assembly. 1c is what says the remaining difference belongs to the
    // model that is being replaced rather than to the one replacing it.
    struct Case { const char *name; double ks, zeta; bool inverted; };
    static const Case kCases[] = {
        { "head, underdamped",   2.0, 0.30, true  },
        { "head, overdamped",    2.0, 1.40, true  },
        { "head, critical",      2.0, 1.00, true  },
        { "head, UNSTABLE k<0",  0.2, 0.30, true  },
        { "head, no stiffness",  1.0, 0.30, true  },   // ks=1 cancels m g d
        { "arm, underdamped",    2.0, 0.30, false },
        { "arm, overdamped",     2.0, 1.40, false },
    };
    auto childOf = [&]( const Case &cs ) {
        twBodyJoint child;
        child.segment          = cs.inverted ? body.segment( twBodySeg::HeadNeck )
                                             : body.compound( twBodySeg::UpperArm, 3 );
        child.freeAxes         = twBodySpherical();
        child.invertedPendulum = cs.inverted;
        child.stiffnessScale   = cs.ks;
        child.dampingRatio     = cs.zeta;
        child.posturalGain     = 0.0;
        child.parentLever      = body.segment( twBodySeg::Trunk ).length;
        return child;
    };
    auto pairOf = [&]( const Case &cs, twBodyChain &ch ) {
        ch.n = 2;
        // The parent exists only to be prescribed. Its own stiffness is
        // nonsense ON PURPOSE: none of it may reach the child's row, and if any
        // of it did, 1a could not agree with a model that has never heard of it.
        ch.joints[0].joint.segment          = body.segment( twBodySeg::Trunk );
        ch.joints[0].joint.invertedPendulum = true;
        ch.joints[0].joint.stiffnessScale   = 17.0;
        ch.joints[0].joint.dampingRatio     = 3.3;
        ch.joints[0].parent     = -1;
        ch.joints[0].prescribed = true;
        ch.joints[1].joint  = childOf( cs );
        ch.joints[1].parent = 0;
    };
    // The requester's own input: a smooth ramp to 25 degrees, then STOP.
    const double kRamp = 0.25, kAmp = 25.0 * kPi / 180.0;
    auto ramp = [&]( double t, double &phi, double &dphi, double &ddphi ) {
        phi = kAmp; dphi = 0.0; ddphi = 0.0;
        if( t >= kRamp ) return;
        const double u = t / kRamp;
        phi   = kAmp * ( u - std::sin( 2.0 * kPi * u ) / ( 2.0 * kPi ) );
        dphi  = kAmp * ( 1.0 - std::cos( 2.0 * kPi * u ) ) / kRamp;
        ddphi = kAmp * 2.0 * kPi * std::sin( 2.0 * kPi * u ) / ( kRamp * kRamp );
    };

    {
        std::printf( "\n-- 1a. STATIC parent: the chain IS twBodyJointStep --\n" );
        for( const Case &cs : kCases ) {
            twBodyJoint child = childOf( cs );
            twBodyChain ch; pairOf( cs, ch );
            twBodyChainState cst; twBodyJointState jst;
            const double phi = 0.30;                    // a leaning, STILL parent
            const double dt = 5e-4;
            twBodyParentMotion p;
            for( int a = 0; a < (int) twBodyAxis::Count; a++ ) {
                p.angle[a] = phi;
                jst.angle[a] = 0.20; jst.vel[a] = -0.5; // a child in mid-swing
                cst.angle[0][a] = phi;
                cst.angle[1][a] = phi + 0.20; cst.vel[1][a] = -0.5;
            }
            double worstA = 0.0, worstV = 0.0, mag = 0.0;
            for( long n = 0; n < (long) ( 1.2 / dt ); n++ ) {
                twBodyJointStep( child, jst, p, nullptr, dt );
                twBodyChainStep( ch, cst, nullptr, dt );
                worstA = std::max( worstA, std::fabs(
                    twBodyChainRelative( ch, cst, 1, twBodyAxis::Flex ) - jst.angle[kFlex] ) );
                worstV = std::max( worstV, std::fabs(
                    twBodyChainRelativeVel( ch, cst, 1, twBodyAxis::Flex ) - jst.vel[kFlex] ) );
                mag = std::max( mag, std::fabs( jst.angle[kFlex] ) );
            }
            std::printf( "  %-20s peak |dtheta| = %.3g, |dvel| = %.3g"
                         "   over |theta| up to %.4f\n",
                         cs.name, worstA, worstV, mag );
            check( worstA < 1e-12 * ( 1.0 + mag ) && worstV < 1e-10 * ( 1.0 + mag ),
                   "1a: the chain reduces to twBodyJointStep exactly" );
            check( mag > 0.05, "...over a trajectory that actually moved" );
        }
    }

    {
        std::printf( "\n-- 1b. MOVING parent, matched state: the two agree to"
                     " O(dt^3) --\n" );
        auto stepGap = [&]( const Case &cs, double dt ) {
            twBodyJoint child = childOf( cs );
            twBodyChain ch; pairOf( cs, ch );
            twBodyChainState cst; twBodyJointState jst;
            double worst = 0.0;
            for( long n = 0; n < (long) ( 1.2 / dt ); n++ ) {
                double phi, dphi, ddphi; ramp( n * dt, phi, dphi, ddphi );
                twBodyParentMotion p;
                for( int a = 0; a < (int) twBodyAxis::Count; a++ ) {
                    p.angle[a] = phi; p.angVel[a] = dphi; p.angAcc[a] = ddphi;
                    cst.angle[0][a] = phi; cst.vel[0][a] = dphi; cst.acc[0][a] = ddphi;
                    // MATCH the state: the chain's absolute = relative + parent.
                    cst.angle[1][a] = phi  + jst.angle[a];
                    cst.vel  [1][a] = dphi + jst.vel[a];
                }
                twBodyJointStep( child, jst, p, nullptr, dt );
                twBodyChainStep( ch, cst, nullptr, dt );
                worst = std::max( worst, std::fabs(
                    twBodyChainRelative( ch, cst, 1, twBodyAxis::Flex ) - jst.angle[kFlex] ) );
            }
            return worst;
        };
        const Case &cs = kCases[0];
        const double h1 = stepGap( cs, 1e-3 ), h2 = stepGap( cs, 5e-4 ),
                     h4 = stepGap( cs, 2.5e-4 );
        std::printf( "  peak per-step |dtheta|: %.4e / %.4e / %.4e"
                     "  (ratios %.2f, %.2f -- THIRD order, measured)\n",
                     h1, h2, h4, h1 / h2, h2 / h4 );
        // Third, not second. The header of this section predicted O(dt^2) and
        // the measurement said 8, so the prediction is corrected rather than
        // the tolerance widened.
        check( h1 / h2 > 6.0 && h1 / h2 < 10.0, "1b: halving dt EIGHTHS the gap" );
        check( h2 / h4 > 6.0 && h2 / h4 < 10.0, "1b: and again" );
    }

    {
        std::printf( "\n-- 1c. and the CHAIN is the exact one --\n" );
        // Subdivision invariance: an integrator that is EXACT for the forcing
        // it is given returns the identical state whether it takes one step of
        // dt or two of dt/2. Nothing is compared with a reference
        // implementation -- the claim is self-contained, which is what makes it
        // an exactness statement rather than an agreement statement.
        //
        // ON THE AXIAL AXIS IT IS EXACT, and that is the axis to make the claim
        // on because it is the LINEAR system with nothing frozen: there is no
        // lever about the long axis (twbodyjoint.h invariant 3), so no
        // centripetal term and no mass coupling, while the joint's own spring
        // and damper still pull the child toward a parent that is MOVING. So
        // this bites the polynomial forcing specifically -- drop the t and
        // t^2/2 rows and the answer stops being subdivision-invariant.
        const double phi0 = 0.10, dphi0 = 1.3, ddphi0 = 9.0, dtc = 4e-3;
        for( const Case &cs : kCases ) {
            twBodyChain ch; pairOf( cs, ch );
            auto run = [&]( int sub, int axis ) {
                twBodyChainState cst;
                for( int a = 0; a < (int) twBodyAxis::Count; a++ ) {
                    cst.angle[0][a] = phi0; cst.vel[0][a] = dphi0; cst.acc[0][a] = ddphi0;
                    cst.angle[1][a] = phi0 + 0.2; cst.vel[1][a] = dphi0 - 0.5;
                }
                for( int i = 0; i < sub; i++ ) twBodyChainStep( ch, cst, nullptr, dtc / sub );
                return cst.angle[1][axis];
            };
            const int kAxial = (int) twBodyAxis::Axial;
            const double one = run( 1, kAxial ), two = run( 2, kAxial ),
                         four = run( 4, kAxial );
            std::printf( "  %-20s axial psi after %.0f ms: %.15f / %.15f / %.15f\n",
                         cs.name, dtc * 1e3, one, two, four );
            near( two,  one, 1e-14 * ( 1.0 + std::fabs( one ) ),
                  "1c: the chain is subdivision-invariant, so it is EXACT" );
            near( four, one, 1e-14 * ( 1.0 + std::fabs( one ) ),
                  "1c: at four sub-steps too" );
        }
        // THE ONE FROZEN COEFFICIENT, named and attributed by MEASUREMENT
        // rather than by assertion. In the sagittal plane the chain is NOT
        // subdivision-invariant, because the centripetal stiffening depends on
        // the parent's rate SQUARED and that rate is held at its top-of-step
        // value -- exactly as twBodyJointStep holds it. Its change across a
        // step is 2*w*phi''*dt, so the residual must be LINEAR in the parent's
        // rate, which no other candidate term is: a coupling error would not
        // move with w at all, and a forcing-order error would be cubic in dt.
        {
            twBodyChain ch; pairOf( kCases[0], ch );
            auto resid = [&]( double dphi ) {
                auto run = [&]( int sub ) {
                    twBodyChainState cst;
                    for( int a = 0; a < (int) twBodyAxis::Count; a++ ) {
                        cst.angle[0][a] = phi0; cst.vel[0][a] = dphi;
                        cst.acc[0][a] = ddphi0;
                        cst.angle[1][a] = phi0 + 0.2; cst.vel[1][a] = dphi - 0.5;
                    }
                    for( int i = 0; i < sub; i++ )
                        twBodyChainStep( ch, cst, nullptr, dtc / sub );
                    return cst.angle[1][kFlex];
                };
                return std::fabs( run( 2 ) - run( 1 ) );
            };
            const double rFast = resid( 1.3 ), rSlow = resid( 0.13 ),
                         rStill = resid( 0.0 );
            std::printf( "  sagittal residual (the FROZEN centripetal term):"
                         " %.3e at phi'=1.30, %.3e at 0.13, %.3e at 0\n",
                         rFast, rSlow, rStill );
            check( rFast / rSlow > 6.0 && rFast / rSlow < 15.0,
                   "1c: the sagittal residual is LINEAR in the parent's rate,"
                   " which identifies it as the centripetal freeze" );
            check( rStill < 0.02 * rFast,
                   "1c: and it nearly vanishes with the parent standing still" );
            check( rFast < 1e-6, "1c: it is small in absolute terms too" );
        }
        // THE CONTROL, and it is what makes 1c mean anything: the model being
        // replaced is NOT subdivision-invariant on the axis where the parent's
        // own angle reaches it, because freezing that angle and rate inside a
        // step is exactly a first-order assumption.
        {
            twBodyJoint child = childOf( kCases[0] );
            auto runJ = [&]( int sub ) {
                twBodyJointState jst;
                for( int a = 0; a < (int) twBodyAxis::Count; a++ )
                    { jst.angle[a] = 0.2; jst.vel[a] = -0.5; }
                for( int i = 0; i < sub; i++ ) {
                    twBodyParentMotion p;
                    const double t = ( dtc / sub ) * i;
                    for( int a = 0; a < (int) twBodyAxis::Count; a++ ) {
                        p.angle[a]  = phi0 + dphi0 * t + 0.5 * ddphi0 * t * t;
                        p.angVel[a] = dphi0 + ddphi0 * t;
                        p.angAcc[a] = ddphi0;
                    }
                    twBodyJointStep( child, jst, p, nullptr, dtc / sub );
                }
                return jst.angle[kFlex];
            };
            const double jo = runJ( 1 ), jt = runJ( 2 );
            std::printf( "  CONTROL twBodyJointStep:      theta %.15f / %.15f"
                         "  (differ by %.3e)\n", jo, jt, std::fabs( jt - jo ) );
            check( std::fabs( jt - jo ) > 1e-9,
                   "1c CONTROL: the one-way model is NOT subdivision-invariant" );
        }
    }

    {
        std::printf( "\n-- 1d. free-running, the trajectories converge, first"
                     " order --\n" );
        auto gap = [&]( double dt ) {
            twBodyJoint child = childOf( kCases[0] );
            twBodyChain ch; pairOf( kCases[0], ch );
            twBodyChainState cst; twBodyJointState jst;
            double worst = 0.0;
            for( long n = 0; n < (long) ( 1.2 / dt ); n++ ) {
                double phi, dphi, ddphi; ramp( n * dt, phi, dphi, ddphi );
                twBodyParentMotion p;
                for( int a = 0; a < (int) twBodyAxis::Count; a++ ) {
                    p.angle[a] = phi; p.angVel[a] = dphi; p.angAcc[a] = ddphi;
                    cst.angle[0][a] = phi; cst.vel[0][a] = dphi; cst.acc[0][a] = ddphi;
                }
                twBodyJointStep( child, jst, p, nullptr, dt );
                twBodyChainStep( ch, cst, nullptr, dt );
                worst = std::max( worst, std::fabs(
                    twBodyChainRelative( ch, cst, 1, twBodyAxis::Flex ) - jst.angle[kFlex] ) );
            }
            return worst;
        };
        const double g1 = gap( 1e-3 ), g2 = gap( 5e-4 ), g4 = gap( 2.5e-4 );
        std::printf( "  peak |dtheta|: %.4e / %.4e / %.4e  (ratios %.3f, %.3f)\n",
                     g1, g2, g4, g1 / g2, g2 / g4 );
        check( g1 / g2 > 1.7 && g1 / g2 < 2.3, "1d: halving dt halves the gap" );
        check( g2 / g4 > 1.7 && g2 / g4 < 2.3, "1d: and again" );
    }

    // --- the four-joint body the plant builds, as a CHAIN ------------------
    enum { JLeg = 0, JTrunk = 1, JArm = 2, JNeck = 3 };
    twBodyChain full;
    {
        full.n = 4;
        full.joints[JLeg].joint.segment          = body.compound( twBodySeg::Thigh, 3 );
        full.joints[JLeg].joint.invertedPendulum = true;
        full.joints[JLeg].joint.parentLever      = 0.0;
        full.joints[JLeg].parent                 = -1;
        full.joints[JTrunk].joint.segment          = body.segment( twBodySeg::Trunk );
        full.joints[JTrunk].joint.invertedPendulum = true;
        full.joints[JTrunk].joint.parentLever      = body.compound( twBodySeg::Thigh, 3 ).length;
        full.joints[JTrunk].parent                 = JLeg;
        full.joints[JArm].joint.segment          = body.compound( twBodySeg::UpperArm, 3 );
        full.joints[JArm].joint.invertedPendulum = false;   // an arm HANGS
        full.joints[JArm].joint.parentLever      = body.segment( twBodySeg::Trunk ).length;
        full.joints[JArm].parent                 = JTrunk;
        full.joints[JNeck].joint.segment          = body.segment( twBodySeg::HeadNeck );
        full.joints[JNeck].joint.invertedPendulum = true;
        full.joints[JNeck].joint.parentLever      = body.segment( twBodySeg::Trunk ).length;
        full.joints[JNeck].parent                 = JTrunk;
        for( int j = 0; j < 4; j++ ) {
            full.joints[j].joint.freeAxes       = twBodySpherical();
            full.joints[j].joint.stiffnessScale = 2.0;
            full.joints[j].joint.dampingRatio   = 0.30;
            full.joints[j].joint.posturalGain   = 1.0;
        }
    }

    // --- 2. THE MASS MATRIX, AS CLOSED FORMS ------------------------------
    {
        std::printf( "\n-- 2. the mass matrix IS the inward pass --\n" );
        double M[16];
        twBodyChainMassMatrix( full, twBodyAxis::Flex, M );
        const twBodySegment head  = body.segment( twBodySeg::HeadNeck );
        const twBodySegment trunk = body.segment( twBodySeg::Trunk );
        const twBodySegment arm   = body.compound( twBodySeg::UpperArm, 3 );
        const double L = trunk.length;

        for( int i = 0; i < 4; i++ )
            for( int j = 0; j < 4; j++ )
                near( M[i * 4 + j], M[j * 4 + i], 1e-12, "the mass matrix is symmetric" );

        const double wantTN = head.mass * L * head.comFromProx;
        std::printf( "  M[trunk][neck] = %.6f kg m^2   (m_head * L * d_head"
                     " = %.6f)\n", M[JTrunk * 4 + JNeck], wantTN );
        near( M[JTrunk * 4 + JNeck], wantTN, 1e-12,
              "the trunk-neck coupling is +m*L*d" );

        const double wantTA = -arm.mass * L * arm.comFromProx;
        std::printf( "  M[trunk][arm]  = %+.6f kg m^2  (-m_arm * L * d_arm"
                     " = %+.6f)  -- NEGATIVE, because an arm hangs\n",
                     M[JTrunk * 4 + JArm], wantTA );
        near( M[JTrunk * 4 + JArm], wantTA, 1e-12,
              "the trunk-arm coupling is NEGATIVE" );
        check( M[JTrunk * 4 + JArm] < 0.0 && M[JTrunk * 4 + JNeck] > 0.0,
               "and the two couplings have OPPOSITE signs" );

        // The diagonal the one-way model omitted entirely.
        const double wantTT = trunk.inertiaProx
                            + ( arm.mass + head.mass ) * L * L;
        std::printf( "  M[trunk][trunk]= %.6f kg m^2   (I_prox %.6f + carried"
                     " %.6f)\n", M[JTrunk * 4 + JTrunk], trunk.inertiaProx,
                     ( arm.mass + head.mass ) * L * L );
        near( M[JTrunk * 4 + JTrunk], wantTT, 1e-12,
              "a trunk carries its arms and its head" );
        check( M[JTrunk * 4 + JTrunk] > 1.5 * trunk.inertiaProx,
               "which is not a small correction" );

        // THE FIGURE THE EXECUTION PLAN SIZED THE GAP WITH -- AND IT WAS 11.4 %,
        // WHICH IS WRONG. Recomputing it here against the measures module gives
        // **10.5 %**: the plan's arithmetic used a head CoM of 0.1231 m above
        // the atlas where twBodyMeasures says 0.11375 m. Its other three inputs
        // -- head 5.175 kg, trunk I 2.81270 kg*m^2, neck lever 0.504 m -- match
        // to every digit it quoted, so the slip is that one number and nothing
        // else. Corrected in the plan and in tw/body's CONTRACT rather than
        // quietly rounded to.
        const double planRatio = wantTN / trunk.inertiaProx;
        const double realRatio = wantTN / M[JTrunk * 4 + JTrunk];
        std::printf( "  reaction ratio: %.4f of the head's angular acceleration"
                     " against the trunk ALONE (the plan said 0.114),\n"
                     "                  %.4f against the trunk as it actually"
                     " is -- carrying two arms and a head\n",
                     planRatio, realRatio );
        near( planRatio, head.mass * head.comFromProx * L / trunk.inertiaProx,
              1e-12, "the reaction ratio is m*d*L over the trunk's own inertia" );
        near( planRatio, 0.1055, 0.0005,
              "and it is 10.5%, not the 11.4% the plan recorded" );
        check( realRatio < planRatio,
               "the honest ratio is SMALLER still, because the trunk is heavier"
               " than that figure assumed" );

        // The AXIAL problem is diagonal -- geometry, not a simplification.
        double Ma[16];
        twBodyChainMassMatrix( full, twBodyAxis::Axial, Ma );
        double offMax = 0.0;
        for( int i = 0; i < 4; i++ )
            for( int j = 0; j < 4; j++ )
                if( i != j ) offMax = std::max( offMax, std::fabs( Ma[i * 4 + j] ) );
        near( offMax, 0.0, 0.0, "the axial mass matrix is exactly diagonal" );
        near( Ma[JNeck * 4 + JNeck], head.inertiaLong, 1e-12,
              "and its diagonal is the LONGITUDINAL inertia" );
    }

    // --- 3. AC4.2: AN ARM SWING COUNTER-ROTATES THE TRUNK ------------------
    {
        std::printf( "\n-- 3. AC4.2: a muscle torque at one joint moves the"
                     " OTHER end of it --\n" );
        // Trunk (root) + arm, stripped to pure inertia: no passive stiffness,
        // no damping, gravity fully compensated. Then the answer to a single
        // shoulder torque is a two-by-two solve and nothing else.
        twBodyChain ch;
        ch.n = 2;
        ch.joints[0].joint          = full.joints[JTrunk].joint;
        ch.joints[0].joint.parentLever = 0.0;
        ch.joints[0].parent         = -1;
        ch.joints[1].joint          = full.joints[JArm].joint;
        ch.joints[1].parent         = 0;
        for( int j = 0; j < 2; j++ ) {
            ch.joints[j].joint.stiffnessScale = 0.0;
            ch.joints[j].joint.dampingRatio   = 0.0;
            ch.joints[j].joint.posturalGain   = 1.0;
        }
        double M[4];
        twBodyChainMassMatrix( ch, twBodyAxis::Flex, M );
        const double tau = 10.0;                       // N*m at the shoulder
        const double det = M[0] * M[3] - M[1] * M[2];
        const double wantTrunk = ( -tau * M[3] - M[1] * tau ) / det;
        const double wantArm   = (  tau * M[0] + M[2] * tau ) / det;

        double t[2 * (int) twBodyAxis::Count] = {};
        t[1 * (int) twBodyAxis::Count + kFlex] = tau;   // the SHOULDER's torque
        twBodyChainState st;
        twBodyChainStep( ch, st, t, 1e-5 );
        std::printf( "  shoulder torque %+.1f N*m -> trunk %+.6f rad/s^2,"
                     " arm %+.6f rad/s^2\n", tau, st.acc[0][kFlex], st.acc[1][kFlex] );
        std::printf( "  closed form of the 2x2:      trunk %+.6f, arm %+.6f\n",
                     wantTrunk, wantArm );
        near( st.acc[0][kFlex], wantTrunk, 1e-9, "the trunk's reaction is the 2x2 solve" );
        near( st.acc[1][kFlex], wantArm,   1e-9, "the arm's own acceleration too" );
        check( std::fabs( st.acc[0][kFlex] ) > 1e-6,
               "AC4.2: the trunk MOVES -- the one-way model's answer is exactly 0" );
        // COUNTER-ROTATION IS A STATEMENT ABOUT PHYSICAL DIRECTION, NOT ABOUT
        // TWO NUMERICAL SIGNS, and reading it off `acc` directly is wrong: the
        // trunk and the arm carry OPPOSITE sigma, so a shared numerical sign is
        // opposite motion. What must be opposed is where the two centres of
        // mass actually go, which is the same cf geometry the mass matrix is
        // built from, written out here so the claim is legible:
        //     x_trunk = -( sigma_t * d_t ) * psi_t
        //     x_arm   = -( sigma_t * L * psi_t + sigma_a * d_a * psi_a )
        const twBodySegment trunkSeg = ch.joints[0].joint.segment;
        const twBodySegment armSeg   = ch.joints[1].joint.segment;
        const double xTrunk = -( trunkSeg.comFromProx * st.acc[0][kFlex] );
        const double xArm   = -( ch.joints[1].joint.parentLever * st.acc[0][kFlex]
                                 - armSeg.comFromProx * st.acc[1][kFlex] );
        std::printf( "  the two centres of mass accelerate %+.4f and %+.4f m/s^2"
                     " sideways\n", xTrunk, xArm );
        check( xTrunk * xArm < 0.0,
               "AC4.2: and it COUNTER-rotates -- the two CoMs go opposite ways" );

        // THE CONTROL, in the same breath: the one-way model given exactly this
        // torque at the shoulder does nothing whatever to the trunk, because it
        // has no term that could.
        twBodyJoint trunkAlone = ch.joints[0].joint;
        twBodyJointState jst;
        const twBodyParentMotion still;
        double none[(int) twBodyAxis::Count] = {};
        twBodyJointStep( trunkAlone, jst, still, none, 1e-5 );
        near( jst.vel[kFlex], 0.0, 0.0,
              "the CONTROL: twBodyJointStep leaves the trunk exactly still" );
    }

    // --- 4. A HEAVIER TRUNK IS A SLOWER TRUNK ------------------------------
    {
        std::printf( "\n-- 4. the carried mass moves the trunk's own"
                     " resonance --\n" );
        // Free ringing, no drive: count zero crossings of the trunk over a
        // window, with the head and arms present and then absent. No constant
        // is asserted -- only that carrying 20 kg at half a metre slows it.
        auto ringHz = [&]( bool carry ) {
            twBodyChain ch = full;
            if( !carry ) ch.n = 2;              // leg + trunk only
            for( int j = 0; j < ch.n; j++ ) ch.joints[j].joint.dampingRatio = 0.02;
            twBodyChainState st;
            st.angle[JTrunk][kFlex] = 0.05;
            const double dt = 2e-4;
            int crossings = 0; double prev = st.angle[JTrunk][kFlex];
            const double T = 6.0;
            for( long n = 0; n < (long) ( T / dt ); n++ ) {
                twBodyChainStep( ch, st, nullptr, dt );
                const double v = twBodyChainRelative( ch, st, JTrunk, twBodyAxis::Flex );
                if( ( prev < 0.0 ) != ( v < 0.0 ) ) crossings++;
                prev = v;
            }
            return crossings / ( 2.0 * T );
        };
        const double withHead = ringHz( true ), alone = ringHz( false );
        std::printf( "  trunk ring: %.3f Hz carrying its arms and head,"
                     " %.3f Hz on its own\n", withHead, alone );
        check( withHead < alone,
               "a trunk that knows what it carries rings SLOWER" );
        check( withHead > 0.05,
               "...and it still rings" );
    }

    std::printf( "\nbody_chain_test: %s\n", g_fail ? "FAIL" : "PASS" );
    return g_fail ? 1 : 0;
}
