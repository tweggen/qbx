#include "tw/body/twbodychain.h"

#include <cmath>
#include <cstring>

namespace {

constexpr int kAxes = (int) twBodyAxis::Count;
constexpr int kMaxN = twBodyChainMaxJoints;
/** The augmented state-space size: angle + rate per unknown, plus the constant
 * row that carries the affine forcing (see expm below). */
constexpr int kMaxS = 2 * kMaxN + 3;

/** +1 for a segment that extends UPWARD from its joint, -1 for one that hangs.
 * The SAME sign gravity uses, and it multiplies both the segment's own CoM
 * offset and the levers to its children -- a child of a hanging segment hangs
 * further. */
inline double sigmaOf( const twBodyJoint &j )
{ return j.invertedPendulum ? +1.0 : -1.0; }

inline double gravMoment( const twBodyJoint &j )
{ return j.segment.mass * twBodyGravity * j.segment.comFromProx; }

/** The postural-tone-compensated fraction, 0..1. */
inline double toneOf( const twBodyJoint &j )
{ return j.posturalGain < 0.0 ? 0.0 : ( j.posturalGain > 1.0 ? 1.0 : j.posturalGain ); }

/** The CoM offset and the lever AS FELT ABOUT `a`. Both vanish about the long
 * axis, and that one fact is what makes the axial problem diagonal: a segment
 * twisting about its own long axis moves neither its own CoM nor its children's
 * joints sideways, so there is nothing to couple. */
inline double comAxis( const twBodyJoint &j, twBodyAxis a )
{ return a == twBodyAxis::Axial ? 0.0 : j.segment.comFromProx; }
inline double leverAxis( const twBodyJoint &j, twBodyAxis a )
{ return a == twBodyAxis::Axial ? 0.0 : j.parentLever; }

/**
 * The LINEARISED horizontal-displacement coefficients: `cf[j][b]` is how much
 * segment j's centre of mass moves sideways per radian of joint b.
 *
 *   b == j            sigma_j * d_j            its own CoM offset
 *   b a proper ancestor  sigma_b * L_{b->next} the lever carrying it along
 *   otherwise         0
 *
 * ONE array, and everything else here is built from it: the mass matrix is
 * `sum_j m_j cf[j][.] cf[j][.]`, and the gravity term is `sum_j m_j cf[j][.]`.
 * That is not a coincidence -- one is the kinetic form and the other the
 * potential, over the same geometry.
 */
void buildCf( const twBodyChain &c, twBodyAxis a, double cf[kMaxN][kMaxN] )
{
    std::memset( cf, 0, sizeof( double ) * kMaxN * kMaxN );
    for( int j = 0; j < c.n; j++ ) {
        cf[j][j] += sigmaOf( c.joints[j].joint ) * comAxis( c.joints[j].joint, a );
        int cur = j;
        while( c.joints[cur].parent >= 0 ) {
            const int p = c.joints[cur].parent;
            // The lever belongs to the CHILD (it is the distance from p up to
            // cur); the SIGN belongs to the parent, whose segment it lies along.
            cf[j][p] += sigmaOf( c.joints[p].joint )
                      * leverAxis( c.joints[cur].joint, a );
            cur = p;
        }
    }
}

// --- small dense linear algebra, fixed size, no allocation -------------------

/** Gauss-Jordan inverse with partial pivoting. False if singular. */
bool invert( const double *A, int n, double *out )
{
    double m[kMaxN][2 * kMaxN];
    for( int i = 0; i < n; i++ ) {
        for( int j = 0; j < n; j++ ) m[i][j] = A[i * n + j];
        for( int j = 0; j < n; j++ ) m[i][n + j] = ( i == j ) ? 1.0 : 0.0;
    }
    for( int col = 0; col < n; col++ ) {
        int piv = col;
        for( int r = col + 1; r < n; r++ )
            if( std::fabs( m[r][col] ) > std::fabs( m[piv][col] ) ) piv = r;
        if( std::fabs( m[piv][col] ) < 1e-14 ) return false;
        if( piv != col )
            for( int j = 0; j < 2 * n; j++ ) { const double t = m[col][j];
                m[col][j] = m[piv][j]; m[piv][j] = t; }
        const double d = m[col][col];
        for( int j = 0; j < 2 * n; j++ ) m[col][j] /= d;
        for( int r = 0; r < n; r++ ) {
            if( r == col ) continue;
            const double f = m[r][col];
            if( f == 0.0 ) continue;
            for( int j = 0; j < 2 * n; j++ ) m[r][j] -= f * m[col][j];
        }
    }
    for( int i = 0; i < n; i++ )
        for( int j = 0; j < n; j++ ) out[i * n + j] = m[i][n + j];
    return true;
}

/**
 * exp(A) by scaling and squaring with a Taylor series.
 *
 * The scaling is what makes the Taylor series legitimate rather than hopeful:
 * A is halved until its max row sum is under 1/2, at which point the series
 * reaches machine precision in about eighteen terms, and the result is squared
 * back. Standard, and adequate at n <= 17 -- a Pade approximant would buy
 * accuracy this problem does not need.
 */
void expm( const double *A, int n, double *E )
{
    double norm = 0.0;
    for( int i = 0; i < n; i++ ) {
        double s = 0.0;
        for( int j = 0; j < n; j++ ) s += std::fabs( A[i * n + j] );
        if( s > norm ) norm = s;
    }
    int sq = 0;
    double scale = 1.0;
    while( norm * scale > 0.5 ) { scale *= 0.5; sq++; if( sq > 60 ) break; }

    double X[kMaxS * kMaxS], T[kMaxS * kMaxS], W[kMaxS * kMaxS];
    for( int i = 0; i < n * n; i++ ) X[i] = A[i] * scale;
    for( int i = 0; i < n; i++ )
        for( int j = 0; j < n; j++ ) { E[i * n + j] = ( i == j ) ? 1.0 : 0.0;
                                       T[i * n + j] = E[i * n + j]; }
    for( int k = 1; k <= 24; k++ ) {
        // T <- T * X / k
        for( int i = 0; i < n; i++ )
            for( int j = 0; j < n; j++ ) {
                double s = 0.0;
                for( int q = 0; q < n; q++ ) s += T[i * n + q] * X[q * n + j];
                W[i * n + j] = s / (double) k;
            }
        double mx = 0.0;
        for( int i = 0; i < n * n; i++ ) { T[i] = W[i]; E[i] += T[i];
            const double av = std::fabs( T[i] ); if( av > mx ) mx = av; }
        if( mx < 1e-19 ) break;
    }
    for( int s = 0; s < sq; s++ ) {
        for( int i = 0; i < n; i++ )
            for( int j = 0; j < n; j++ ) {
                double v = 0.0;
                for( int q = 0; q < n; q++ ) v += E[i * n + q] * E[q * n + j];
                W[i * n + j] = v;
            }
        std::memcpy( E, W, sizeof( double ) * n * n );
    }
}

} // namespace

double twBodyChainRelative( const twBodyChain &c, const twBodyChainState &st,
                            int joint, twBodyAxis a )
{
    if( joint < 0 || joint >= c.n ) return 0.0;
    const int p = c.joints[joint].parent;
    return st.angle[joint][(int) a] - ( p >= 0 ? st.angle[p][(int) a] : 0.0 );
}

double twBodyChainRelativeVel( const twBodyChain &c, const twBodyChainState &st,
                               int joint, twBodyAxis a )
{
    if( joint < 0 || joint >= c.n ) return 0.0;
    const int p = c.joints[joint].parent;
    return st.vel[joint][(int) a] - ( p >= 0 ? st.vel[p][(int) a] : 0.0 );
}

void twBodyChainMassMatrix( const twBodyChain &c, twBodyAxis a, double *M )
{
    const int n = c.n;
    for( int i = 0; i < n * n; i++ ) M[i] = 0.0;
    if( n <= 0 ) return;
    double cf[kMaxN][kMaxN];
    buildCf( c, a, cf );
    for( int j = 0; j < n; j++ ) {
        const twBodyJoint &jj = c.joints[j].joint;
        const double m = jj.segment.mass;
        for( int b1 = 0; b1 < n; b1++ ) {
            if( cf[j][b1] == 0.0 ) continue;
            for( int b2 = 0; b2 < n; b2++ )
                M[b1 * n + b2] += m * cf[j][b1] * cf[j][b2];
        }
        // The segment's inertia ABOUT ITS OWN CoM, which the cf form does not
        // carry: I_com = I_prox - m*d^2 transversely, and I_long about the long
        // axis, where cf is zero and this is the whole diagonal.
        const double d = comAxis( jj, a );
        M[j * n + j] += twBodyJointInertia( jj, a ) - m * d * d;
    }
}

void twBodyChainStep( const twBodyChain &c, twBodyChainState &st,
                      const double *activeTorque, double dt )
{
    const int n = c.n;
    if( n <= 0 || n > kMaxN || dt <= 0.0 ) return;

    for( int ai = 0; ai < kAxes; ai++ ) {
        const twBodyAxis ax = (twBodyAxis) ai;

        // --- who is an unknown, who is welded, who is imposed --------------
        // ONE mapping covers all three. A CONSTRAINED axis is a rigid weld to
        // its parent (relative angle exactly zero, as twBodyJointStep holds
        // it), which is expressed by sharing the parent's slot -- so its mass
        // merges into the parent's row and column automatically, rather than
        // its segment quietly vanishing from the body.
        int slot[kMaxN];
        double knownA[kMaxN] = {}, knownV[kMaxN] = {}, knownAcc[kMaxN] = {};
        int q = 0;
        for( int j = 0; j < n; j++ ) {
            const twBodyChainJoint &cj = c.joints[j];
            if( cj.prescribed ) {
                slot[j] = -1;
                knownA[j] = st.angle[j][ai]; knownV[j] = st.vel[j][ai];
                knownAcc[j] = st.acc[j][ai];
            } else if( !twBodyAxisIsFree( cj.joint, ax ) ) {
                const int p = cj.parent;
                if( p < 0 ) { slot[j] = -1; }            // welded to ground
                else {
                    slot[j] = slot[p];
                    if( slot[p] < 0 ) { knownA[j] = knownA[p];
                        knownV[j] = knownV[p]; knownAcc[j] = knownAcc[p]; }
                }
            } else {
                slot[j] = q++;
            }
        }

        // --- assemble the full-coordinate M, C, K and f --------------------
        double cf[kMaxN][kMaxN];
        buildCf( c, ax, cf );
        double M[kMaxN * kMaxN] = {}, C[kMaxN * kMaxN] = {}, K[kMaxN * kMaxN] = {};
        double f[kMaxN] = {};

        for( int j = 0; j < n; j++ ) {
            const twBodyJoint &jj = c.joints[j].joint;
            const double m = jj.segment.mass;
            for( int b1 = 0; b1 < n; b1++ ) {
                if( cf[j][b1] == 0.0 ) continue;
                for( int b2 = 0; b2 < n; b2++ )
                    M[b1 * n + b2] += m * cf[j][b1] * cf[j][b2];
            }
            const double d = comAxis( jj, ax );
            M[j * n + j] += twBodyJointInertia( jj, ax ) - m * d * d;

            // GRAVITY, diagonal in absolute angles -- which is the entire
            // reason these coordinates were chosen. The moment joint b holds is
            // its own segment's plus everything it carries, and postural tone
            // cancels that whole moment (tw/body CONTRACT 13).
            double gsum = 0.0;
            for( int jj2 = 0; jj2 < n; jj2++ )
                gsum += c.joints[jj2].joint.segment.mass * cf[jj2][j];
            K[j * n + j] += -twBodyGravity * gsum * ( 1.0 - toneOf( jj ) );

            // The joint's own spring and damper act on the RELATIVE angle, so
            // they are the off-diagonals. CENTRIPETAL stiffening rides on the
            // spring with the SAME signed coupling the mass matrix carries --
            // an inverted child flung outward is pushed back toward straight,
            // a hanging one is pushed away from plumb.
            const int p = c.joints[j].parent;
            const double wPar = p >= 0 ? st.vel[p][ai] : 0.0;
            const double sigProd = ( p >= 0 ? sigmaOf( c.joints[p].joint ) : 1.0 )
                                 * sigmaOf( jj );
            const double kj = jj.stiffnessScale * gravMoment( jj )
                            + sigProd * m * comAxis( jj, ax ) * leverAxis( jj, ax )
                              * wPar * wPar;
            const double cjd = twBodyJointDamping( jj, ax );
            K[j * n + j] += kj;   C[j * n + j] += cjd;
            if( p >= 0 ) {
                K[p * n + p] += kj;   C[p * n + p] += cjd;
                K[j * n + p] -= kj;   C[j * n + p] -= cjd;
                K[p * n + j] -= kj;   C[p * n + j] -= cjd;
            }

            // THE ACTIVE TORQUE, AND THIS IS THE INWARD PASS FOR A MUSCLE.
            // It acts on the relative angle, so it appears positive on this
            // joint's row and NEGATIVE on its parent's. That one minus sign is
            // what lets an arm swing counter-rotate a trunk.
            if( activeTorque ) {
                const double t = activeTorque[j * kAxes + ai];
                f[j] += t;
                if( p >= 0 ) f[p] -= t;
            }
        }

        // --- reduce onto the unknowns --------------------------------------
        double Mr[kMaxN * kMaxN] = {}, Cr[kMaxN * kMaxN] = {}, Kr[kMaxN * kMaxN] = {};
        // THE FORCING IS A QUADRATIC IN TIME, NOT A CONSTANT, and that is the
        // whole content of "prescribed". A parent with a known acceleration
        // MOVES within the step -- phi(t) = phi + phi'*t + phi''*t^2/2 -- so the
        // stiffness and damping it pulls the unknowns through move with it.
        // Freezing them at the top of the step is what makes the one-way
        // twBodyJointStep only FIRST-ORDER accurate under an accelerating
        // parent (body_chain_test 1b measures the two converging); carrying the
        // polynomial makes this exact for the trajectory a constant phi''
        // actually describes, which is what a gate can be written against.
        double fr0[kMaxN] = {}, fr1[kMaxN] = {}, fr2[kMaxN] = {};
        for( int b1 = 0; b1 < n; b1++ ) {
            const int s1 = slot[b1];
            if( s1 < 0 ) continue;
            fr0[s1] += f[b1];
            for( int b2 = 0; b2 < n; b2++ ) {
                const int s2 = slot[b2];
                if( s2 >= 0 ) {
                    Mr[s1 * q + s2] += M[b1 * n + b2];
                    Cr[s1 * q + s2] += C[b1 * n + b2];
                    Kr[s1 * q + s2] += K[b1 * n + b2];
                } else {
                    // A PRESCRIBED (or ground-welded) column is known, so its
                    // contribution is a force, not a coefficient.
                    fr0[s1] -= M[b1 * n + b2] * knownAcc[b2]
                             + C[b1 * n + b2] * knownV[b2]
                             + K[b1 * n + b2] * knownA[b2];
                    fr1[s1] -= C[b1 * n + b2] * knownAcc[b2]
                             + K[b1 * n + b2] * knownV[b2];
                    fr2[s1] -= K[b1 * n + b2] * knownAcc[b2];
                }
            }
        }

        // --- advance the imposed and welded joints -------------------------
        // A PRESCRIBED joint moves too, by its own given acceleration, so the
        // state it leaves is consistent with the forcing the unknowns just
        // saw. A caller driving a trajectory simply overwrites it next step.
        for( int j = 0; j < n; j++ ) {
            if( slot[j] >= 0 ) continue;
            st.angle[j][ai] = knownA[j] + knownV[j] * dt
                            + 0.5 * knownAcc[j] * dt * dt;
            st.vel[j][ai]   = knownV[j] + knownAcc[j] * dt;
            st.acc[j][ai]   = knownAcc[j];
        }
        if( q == 0 ) continue;

        double Minv[kMaxN * kMaxN];
        if( !invert( Mr, q, Minv ) ) continue;   // a degenerate segment: leave it

        double x0[kMaxN], v0[kMaxN];
        for( int j = 0; j < n; j++ )
            if( slot[j] >= 0 ) { x0[slot[j]] = st.angle[j][ai];
                                 v0[slot[j]] = st.vel[j][ai]; }

        // The acceleration AT THE TOP OF THE STEP: Mr^-1 (fr - Cr v - Kr x).
        // Reported rather than differenced, because it is what a reaction
        // measurement wants to read and a difference would carry the step.
        double rhs[kMaxN], acc[kMaxN];
        for( int i = 0; i < q; i++ ) {
            double s = fr0[i];
            for( int j = 0; j < q; j++ )
                s -= Cr[i * q + j] * v0[j] + Kr[i * q + j] * x0[j];
            rhs[i] = s;
        }
        for( int i = 0; i < q; i++ ) {
            double s = 0.0;
            for( int j = 0; j < q; j++ ) s += Minv[i * q + j] * rhs[j];
            acc[i] = s;
        }

        // --- the exact step: an augmented state-space matrix exponential ----
        // xdot = A x + g  becomes  d/dt [x; 1] = [[A, g],[0, 0]] [x; 1], whose
        // exponential gives BOTH the homogeneous propagator and the exact
        // integral of the affine forcing -- and does not care whether A is
        // singular, which a k = 0 joint makes it.
        const int S = 2 * q + 3;
        double A[kMaxS * kMaxS] = {}, E[kMaxS * kMaxS];
        for( int i = 0; i < q; i++ ) A[i * S + ( q + i )] = dt;
        for( int i = 0; i < q; i++ ) {
            double mk[kMaxN], mc[kMaxN], mf[3] = { 0.0, 0.0, 0.0 };
            for( int j = 0; j < q; j++ ) {
                double sk = 0.0, sc = 0.0;
                for( int r = 0; r < q; r++ ) {
                    sk += Minv[i * q + r] * Kr[r * q + j];
                    sc += Minv[i * q + r] * Cr[r * q + j];
                }
                mk[j] = sk; mc[j] = sc;
            }
            for( int r = 0; r < q; r++ ) {
                mf[0] += Minv[i * q + r] * fr0[r];
                mf[1] += Minv[i * q + r] * fr1[r];
                mf[2] += Minv[i * q + r] * fr2[r];
            }
            for( int j = 0; j < q; j++ ) {
                A[( q + i ) * S + j ]       = -mk[j] * dt;
                A[( q + i ) * S + ( q + j )] = -mc[j] * dt;
            }
            for( int k = 0; k < 3; k++ )
                A[( q + i ) * S + ( 2 * q + k )] = mf[k] * dt;
        }
        // The nilpotent tail: d/dt (1, t, t^2/2) = (0, 1, t). Seeded (1, 0, 0),
        // so the forcing basis is generated exactly rather than sampled.
        A[( 2 * q + 1 ) * S + ( 2 * q + 0 )] = dt;
        A[( 2 * q + 2 ) * S + ( 2 * q + 1 )] = dt;
        expm( A, S, E );

        double xn[kMaxN], vn[kMaxN];
        for( int i = 0; i < q; i++ ) {
            double s = E[i * S + ( 2 * q )];
            for( int j = 0; j < q; j++ ) s += E[i * S + j] * x0[j]
                                            + E[i * S + ( q + j )] * v0[j];
            xn[i] = s;
        }
        for( int i = 0; i < q; i++ ) {
            const int r = q + i;
            double s = E[r * S + ( 2 * q )];
            for( int j = 0; j < q; j++ ) s += E[r * S + j] * x0[j]
                                            + E[r * S + ( q + j )] * v0[j];
            vn[i] = s;
        }

        for( int j = 0; j < n; j++ ) {
            const int s = slot[j];
            if( s < 0 ) continue;
            st.angle[j][ai] = xn[s];
            st.vel[j][ai]   = vn[s];
            st.acc[j][ai]   = acc[s];
        }

        // --- hard range of motion, on the RELATIVE angle -------------------
        // Dissipative, exactly as twBodyJointStep's is: a ligament stop does
        // not bounce the segment back. Applied AFTER the coupled solve, which
        // means a clamp is not fed back into the parent within the same step --
        // stated rather than hidden, and the same approximation the one-way
        // model made.
        for( int j = 0; j < n; j++ ) {
            if( slot[j] < 0 ) continue;
            const double lim = c.joints[j].joint.limitRad[ai];
            if( lim <= 0.0 ) continue;
            const double rel = twBodyChainRelative( c, st, j, ax );
            if( std::fabs( rel ) <= lim ) continue;
            const double sign = rel > 0.0 ? 1.0 : -1.0;
            const int p = c.joints[j].parent;
            st.angle[j][ai] = ( p >= 0 ? st.angle[p][ai] : 0.0 ) + sign * lim;
            const double relV = twBodyChainRelativeVel( c, st, j, ax );
            if( relV * sign > 0.0 ) st.vel[j][ai] = p >= 0 ? st.vel[p][ai] : 0.0;
            st.limitHits[j]++;
        }
    }
}
