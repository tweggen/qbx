#include "tw/body/twbodyjoint.h"

#include <cmath>

namespace {

constexpr double kPi = 3.14159265358979323846;

/**
 * How strongly the PARENT's angular acceleration drives this joint.
 *
 *     I*th'' + c*th' + k*th = -( I + m*d*L ) * phi''
 *
 * The two terms are physically distinct and both are the requester's effect:
 *   I*phi''      -- the reaction to the parent's own ANGULAR deceleration;
 *   m*d*L*phi''  -- `m*d*a_base`, the parent's joint accelerating this one
 *                   LINEARLY through the lever L.
 * Dividing by I gives the coefficient on phi'' in the angular equation.
 */
inline double driveCoef( const twBodyJoint &j )
{
    if( j.segment.inertiaProx <= 0.0 ) return 0.0;
    return 1.0 + ( j.segment.mass * j.segment.comFromProx * j.parentLever )
                 / j.segment.inertiaProx;
}

/**
 * EXACT step of  th'' + 2*z*w*th' + w^2*th = u  for u constant over dt.
 *
 * Solved as a decay about the steady state th_ss = u/w^2, which is what makes
 * it exact rather than merely stable: the homogeneous part is a pure damped
 * oscillation and has a closed form in all three damping regimes.
 */
void exactStep( double &th, double &vel, double w, double z, double u, double dt )
{
    if( w < 1e-9 ) {
        // No stiffness: pure double integration, also exact for constant u.
        // (A damping-only joint; the trapezoid below would be wrong here.)
        const double a = u - 2.0 * z * w * vel;
        th  += vel * dt + 0.5 * a * dt * dt;
        vel += a * dt;
        return;
    }
    const double thSs = u / ( w * w );
    double e  = th - thSs;      // displacement from the steady state
    double ed = vel;
    const double sig = z * w;
    const double dec = std::exp( -sig * dt );

    if( z < 1.0 - 1e-9 ) {                       // UNDERDAMPED -- it rings
        const double wd = w * std::sqrt( 1.0 - z * z );
        const double c  = std::cos( wd * dt ), s = std::sin( wd * dt );
        const double eN  = dec * ( e * c + ( ( ed + sig * e ) / wd ) * s );
        const double edN = dec * ( ed * c - ( ( w * w * e + sig * ed ) / wd ) * s );
        e = eN; ed = edN;
    } else if( z > 1.0 + 1e-9 ) {                // OVERDAMPED -- it crawls
        const double wd = w * std::sqrt( z * z - 1.0 );
        const double c  = std::cosh( wd * dt ), s = std::sinh( wd * dt );
        const double eN  = dec * ( e * c + ( ( ed + sig * e ) / wd ) * s );
        const double edN = dec * ( ed * c - ( ( w * w * e + sig * ed ) / wd ) * s );
        e = eN; ed = edN;
    } else {                                      // CRITICAL
        const double eN  = dec * ( e + ( ed + sig * e ) * dt );
        const double edN = dec * ( ed - ( w * w * e + sig * ed ) * dt );
        e = eN; ed = edN;
    }
    th  = thSs + e;
    vel = ed;
}

} // namespace

double twBodyJointStiffness( const twBodyJoint &j )
{
    const double gravityMoment =
        j.segment.mass * twBodyGravity * j.segment.comFromProx;
    // Gravity RESTORES a hanging segment and DESTABILISES one whose mass sits
    // above the joint. Treating it as restoring in both cases makes an upright
    // head appear stable with no muscle tone at all, which is the opposite of
    // true -- an inverted segment needs passive tissue exceeding its own
    // gravity moment before it can hold itself up.
    const double sign = j.invertedPendulum ? -1.0 : +1.0;
    return ( j.stiffnessScale + sign ) * gravityMoment;
}

bool twBodyAxisIsFree( const twBodyJoint &j, twBodyAxis a )
{
    return ( j.freeAxes & twBodyAxisBit( a ) ) != 0;
}

double twBodyJointNaturalHz( const twBodyJoint &j, twBodyAxis a )
{
    if( !twBodyAxisIsFree( j, a ) ) return 0.0;
    if( j.segment.inertiaProx <= 0.0 ) return 0.0;
    const double k = twBodyJointStiffness( j );
    if( k <= 0.0 ) return 0.0;
    return std::sqrt( k / j.segment.inertiaProx ) / ( 2.0 * kPi );
}

void twBodyJointStep( const twBodyJoint &j, twBodyJointState &st,
                      const double parentAngAcc[(int) twBodyAxis::Count],
                      const double *muscleTorque, double dt )
{
    if( j.segment.inertiaProx <= 0.0 || dt <= 0.0 ) return;
    const double I     = j.segment.inertiaProx;
    const double k     = twBodyJointStiffness( j );
    // A non-positive stiffness is a joint that cannot hold itself up. Rather
    // than take sqrt of a negative, treat it as free (w = 0) -- the segment
    // then simply falls under whatever drives it, which is the honest
    // behaviour and is what the instability assertion checks.
    const double w     = k > 0.0 ? std::sqrt( k / I ) : 0.0;
    const double z     = j.dampingRatio;
    const double coef  = driveCoef( j );

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
        double u = -coef * parentAngAcc[i];
        if( muscleTorque ) u += muscleTorque[i] / I;
        exactStep( st.angle[i], st.vel[i], w, z, u, dt );

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

double twBodyJointEnergy( const twBodyJoint &j, const twBodyJointState &st,
                          twBodyAxis a )
{
    const int i = (int) a;
    if( !twBodyAxisIsFree( j, a ) ) return 0.0;
    const double I = j.segment.inertiaProx;
    const double k = twBodyJointStiffness( j );
    return 0.5 * I * st.vel[i] * st.vel[i] + 0.5 * k * st.angle[i] * st.angle[i];
}
