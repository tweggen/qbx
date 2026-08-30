#include "tw/body/twbodyjoint.h"

#include <cmath>

namespace {

constexpr double kPi = 3.14159265358979323846;

/** The segment's own gravity moment m*g*d -- the natural torque unit for this
 * joint, and the unit stiffnessScale is expressed in. Always positive. */
inline double gravityMoment( const twBodyJoint &j )
{ return j.segment.mass * twBodyGravity * j.segment.comFromProx; }

/** The gravity moment ABOUT `a`. Zero about the long axis: twisting a segment
 * neither raises nor lowers its centre of mass, so gravity exerts no moment
 * there. Every consequence of gravity -- the stiffness term, the phi forcing,
 * the inverted-pendulum threshold -- follows this and so is absent for twist. */
inline double gravityMomentAxis( const twBodyJoint &j, twBodyAxis a )
{ return a == twBodyAxis::Axial ? 0.0 : gravityMoment( j ); }

/** The UNCOMPENSATED fraction of the gravity moment about `a` -- what the
 * equation actually sees. Postural tone is feedback on the same absolute angle
 * gravity acts on, so cancelling a fraction of it is exactly scaling BOTH
 * gravity terms (the stiffness one and the parent-angle forcing one) by the
 * same (1 - gain). Applying it here, in one place, is what makes the
 * cancellation exact at any dt rather than a per-step approximation. */
inline double gravityMomentNet( const twBodyJoint &j, twBodyAxis a )
{
    const double g = j.posturalGain < 0.0 ? 0.0
                   : ( j.posturalGain > 1.0 ? 1.0 : j.posturalGain );
    return ( 1.0 - g ) * gravityMomentAxis( j, a );
}

/** The sign of the gravity torque on the child's ABSOLUTE angle. An inverted
 * segment is pushed further over (+); a hanging one is pulled back (-). One
 * sign, used in two places -- as -sign on the stiffness and as +sign on the
 * parent-angle forcing -- which is what keeps those two consistent. */
inline double gravitySign( const twBodyJoint &j )
{ return j.invertedPendulum ? +1.0 : -1.0; }

/** The lever from the parent joint to this one AS FELT ABOUT `a`. Zero about
 * the long axis: this joint sits ON the parent's long axis, so a parent
 * twisting about it produces no linear acceleration here at all. */
inline double leverAxis( const twBodyJoint &j, twBodyAxis a )
{ return a == twBodyAxis::Axial ? 0.0 : j.parentLever; }

/**
 * EXACT step of  I*th'' + c*th' + k*th = tau  for tau constant over dt, in ALL
 * FOUR regimes including k < 0.
 *
 * Solved as a decay about the steady state th_ss = tau/k, which is what makes
 * it exact rather than merely stable. Writing sig = c/(2I) and w2 = k/I, the
 * characteristic roots are -sig +/- sqrt(sig^2 - w2), so ONE discriminant
 * decides the branch and the sign of k is not a special case -- it merely
 * makes the discriminant large enough that one root turns positive and the
 * solution grows. That is the honest behaviour of a joint that cannot hold
 * itself up, and it is what the first build lost by folding k < 0 into "no
 * stiffness at all": a head below its stability threshold, displaced and left
 * alone, sat exactly where it was put forever instead of falling over.
 */
void exactStep( double &th, double &vel, double I, double k, double c,
                double tau, double dt )
{
    if( I <= 0.0 || dt <= 0.0 ) return;
    const double sig = c / ( 2.0 * I );
    const double w2  = k / I;
    const double u   = tau / I;

    // No stiffness at all: th'' + 2*sig*th' = u. There is no steady-state
    // ANGLE to decay about (the joint has no preferred position), so this is
    // integrated directly -- exactly, damping included. The first build's
    // version dropped the damping here, which was harmless only because its
    // damping was itself proportional to sqrt(k) and so vanished with it.
    if( std::fabs( w2 ) < 1e-12 ) {
        if( sig > 1e-12 ) {
            const double vSs = u / ( 2.0 * sig );
            const double dec = std::exp( -2.0 * sig * dt );
            const double dv  = vel - vSs;
            th  += vSs * dt + dv * ( 1.0 - dec ) / ( 2.0 * sig );
            vel  = vSs + dv * dec;
        } else {
            th  += vel * dt + 0.5 * u * dt * dt;
            vel += u * dt;
        }
        return;
    }

    const double thSs = u / w2;      // the equilibrium; NEGATIVE k puts it on
    double e  = th - thSs;           // the far side, which is correct -- an
    double ed = vel;                 // unstable joint runs AWAY from it.
    const double dec  = std::exp( -sig * dt );
    const double disc = sig * sig - w2;

    if( disc < -1e-12 ) {                        // UNDERDAMPED -- it rings
        const double wd = std::sqrt( -disc );
        const double c1 = std::cos( wd * dt ), s1 = std::sin( wd * dt );
        const double eN  = dec * ( e * c1 + ( ( ed + sig * e ) / wd ) * s1 );
        const double edN = dec * ( ed * c1 - ( ( w2 * e + sig * ed ) / wd ) * s1 );
        e = eN; ed = edN;
    } else if( disc > 1e-12 ) {                  // OVERDAMPED, or k < 0
        // The same closed form with cosh/sinh. For k < 0, disc > sig^2, so
        // wd > sig and the root -sig + wd is POSITIVE: the solution diverges
        // as cosh, which is the segment falling over.
        const double wd = std::sqrt( disc );
        const double c1 = std::cosh( wd * dt ), s1 = std::sinh( wd * dt );
        const double eN  = dec * ( e * c1 + ( ( ed + sig * e ) / wd ) * s1 );
        const double edN = dec * ( ed * c1 - ( ( w2 * e + sig * ed ) / wd ) * s1 );
        e = eN; ed = edN;
    } else {                                      // CRITICAL
        const double eN  = dec * ( e + ( ed + sig * e ) * dt );
        const double edN = dec * ( ed - ( w2 * e + sig * ed ) * dt );
        e = eN; ed = edN;
    }
    th  = thSs + e;
    vel = ed;
}

} // namespace

double twBodyJointInertia( const twBodyJoint &j, twBodyAxis a )
{
    // Twist is about the segment's OWN long axis, on which its centre of mass
    // sits -- so there is no parallel-axis term and the inertia is a different,
    // much smaller number than the transverse one. Reusing inertiaProx here
    // made a head as slow to twist as it is to nod, which it is not.
    return a == twBodyAxis::Axial ? j.segment.inertiaLong : j.segment.inertiaProx;
}

double twBodyJointStiffness( const twBodyJoint &j, twBodyAxis a )
{
    // Passive tissue, in units of the segment's own gravity moment. About the
    // long axis that unit is BORROWED (there is no gravity moment there) and
    // no gravity term is added or subtracted -- see the header.
    const double passive = j.stiffnessScale * gravityMoment( j );
    return passive - gravitySign( j ) * gravityMomentNet( j, a );
}

double twBodyJointDamping( const twBodyJoint &j, twBodyAxis a )
{
    const double I = twBodyJointInertia( j, a );
    if( I <= 0.0 ) return 0.0;
    // The REFERENCE stiffness: passive tissue plus the MAGNITUDE of the
    // gravity moment. Positive whichever side the mass sits, so c neither
    // vanishes as an inverted joint approaches its threshold nor moves when
    // the inverted flag is flipped -- and, crucially, it does not move when
    // the stiffness is modulated per step (centripetally now, by
    // co-contraction once C4b lands). Damping is a property of tissue, not a
    // shadow of stiffness.
    const double kRef = j.stiffnessScale * gravityMoment( j )
                        + gravityMomentAxis( j, a );
    if( kRef <= 0.0 ) return 0.0;
    return 2.0 * j.dampingRatio * std::sqrt( kRef / I ) * I;
}

double twBodyJointStiffnessAt( const twBodyJoint &j, twBodyAxis a,
                               const twBodyParentMotion &p )
{
    const double k = twBodyJointStiffness( j, a );
    const double L = leverAxis( j, a );
    if( L <= 0.0 ) return k;
    // CENTRIPETAL stiffening. A segment hanging off a rotating parent is
    // flung outward, and the restoring component of that scales with the
    // parent's angular RATE squared -- so it always adds stiffness, and it
    // grows as f^2. Small at a slow sway, dominant at 2-4 Hz.
    const double w = p.angVel[(int) a];
    return k + j.segment.mass * j.segment.comFromProx * L * w * w;
}

bool twBodyAxisIsFree( const twBodyJoint &j, twBodyAxis a )
{
    return ( j.freeAxes & twBodyAxisBit( a ) ) != 0;
}

double twBodyJointNaturalHz( const twBodyJoint &j, twBodyAxis a )
{
    if( !twBodyAxisIsFree( j, a ) ) return 0.0;
    const double I = twBodyJointInertia( j, a );
    if( I <= 0.0 ) return 0.0;
    const double k = twBodyJointStiffness( j, a );
    if( k <= 0.0 ) return 0.0;
    return std::sqrt( k / I ) / ( 2.0 * kPi );
}

namespace {

/** The constant part of the forcing about `a`, N*m -- everything on the right
 * of the equation. Shared by the step and by the equilibrium query so the two
 * can never disagree about what the joint is being pulled by. */
double forcingTorque( const twBodyJoint &j, twBodyAxis a,
                      const twBodyParentMotion &p, const double *activeTorque )
{
    const int i = (int) a;
    const double I = twBodyJointInertia( j, a );
    const double L = leverAxis( j, a );
    // The parent's acceleration, through this joint's own inertia AND through
    // the lever. The two terms are physically distinct and both are the
    // requester's effect:
    //   I*phi''      -- the reaction to the parent's own ANGULAR deceleration;
    //   m*d*L*phi''  -- `m*d*a_base`, the parent's joint accelerating this one
    //                   LINEARLY through the lever L.
    // About the long axis L is zero and only the first survives: a segment
    // resists being twisted by its own inertia alone.
    double tau = -( I + j.segment.mass * j.segment.comFromProx * L ) * p.angAcc[i];
    // GRAVITY ON THE PARENT'S ANGLE. Gravity acts on the child's ABSOLUTE
    // angle phi + theta; the theta half is the stiffness term, and THIS is the
    // other half. Omitting it is not a refinement: without it a freely hanging
    // arm settles parallel to a leaning trunk instead of hanging plumb.
    tau += gravitySign( j ) * gravityMomentNet( j, a ) * p.angle[i];
    if( activeTorque ) tau += activeTorque[i];
    return tau;
}

} // namespace

double twBodyJointEquilibrium( const twBodyJoint &j, twBodyAxis a,
                               const twBodyParentMotion &p,
                               const double *activeTorque )
{
    if( !twBodyAxisIsFree( j, a ) ) return 0.0;
    const double k = twBodyJointStiffnessAt( j, a, p );
    if( std::fabs( k ) < 1e-12 ) return 0.0;   // no preferred position
    // Only the parent's ANGLE and any muscle torque contribute to where the
    // joint RESTS; its acceleration is transient by definition.
    twBodyParentMotion still = p;
    for( int i = 0; i < (int) twBodyAxis::Count; i++ ) still.angAcc[i] = 0.0;
    return forcingTorque( j, a, still, activeTorque ) / k;
}

void twBodyJointStep( const twBodyJoint &j, twBodyJointState &st,
                      const twBodyParentMotion &parent,
                      const double *activeTorque, double dt )
{
    if( dt <= 0.0 ) return;

    for( int i = 0; i < (int) twBodyAxis::Count; i++ ) {
        const twBodyAxis ax = (twBodyAxis) i;
        if( !twBodyAxisIsFree( j, ax ) ) {
            // A CONSTRAINED axis does not move, whatever drives it. Not "very
            // stiff" -- zero. That is the difference between a hinge and a
            // ball joint somebody tightened, and it is asserted, not assumed.
            st.angle[i] = 0.0;
            st.vel[i]   = 0.0;
            continue;
        }
        const double I = twBodyJointInertia( j, ax );
        if( I <= 0.0 ) continue;
        const double k   = twBodyJointStiffnessAt( j, ax, parent );
        const double c   = twBodyJointDamping( j, ax );
        const double tau = forcingTorque( j, ax, parent, activeTorque );
        exactStep( st.angle[i], st.vel[i], I, k, c, tau, dt );

        // Hard range of motion: the stop is DISSIPATIVE, not springy. A
        // ligament does not bounce the segment back.
        const double lim = j.limitRad[i];
        if( lim > 0.0 && std::fabs( st.angle[i] ) > lim ) {
            const double sign = st.angle[i] > 0.0 ? 1.0 : -1.0;
            st.angle[i] = sign * lim;
            if( st.vel[i] * sign > 0.0 ) st.vel[i] = 0.0;
            st.limitHits++;
        }
    }
}

double twBodyPosturalTorque( const twBodyJoint &j, twBodyAxis a,
                             const twBodyParentMotion &p,
                             const twBodyJointState &st )
{
    if( !twBodyAxisIsFree( j, a ) ) return 0.0;
    const int i = (int) a;
    // What muscle supplies is exactly the compensated part of the gravity
    // torque, negated. LINEARISED in the absolute angle, matching what the
    // coefficients apply -- a compensation computed against the true sine
    // would not cancel what the equation integrates, and the two must agree.
    const double compensated = gravityMomentAxis( j, a ) - gravityMomentNet( j, a );
    const double absolute    = p.angle[i] + st.angle[i];
    return -gravitySign( j ) * compensated * absolute;
}

double twBodyJointEnergy( const twBodyJoint &j, const twBodyJointState &st,
                          twBodyAxis a )
{
    const int i = (int) a;
    if( !twBodyAxisIsFree( j, a ) ) return 0.0;
    const double I = twBodyJointInertia( j, a );
    const double k = twBodyJointStiffness( j, a );
    return 0.5 * I * st.vel[i] * st.vel[i] + 0.5 * k * st.angle[i] * st.angle[i];
}
