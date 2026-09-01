#ifndef _TWBODYCHAIN_H_
#define _TWBODYCHAIN_H_

#include <cstdint>

#include "tw/body/twbodyjoint.h"

/**
 * Proposal 44 C8 -- THE INWARD PASS: a child's reaction acts back on its
 * parent.
 *
 * Pure, deterministic: no engine state, no I/O, no Qt, no threads, no rand().
 *
 * ============================ WHY THIS EXISTS ============================
 *
 * The requester's concern, 2026-08-31, and it was exactly right:
 *
 *   > Inertia of head cannot be converted to movement unbounded because of the
 *   > limited length of the neck, hence is converted in both angular momentum
 *   > and angular/linear momentum for the body.
 *
 * `twBodyJointStep` is ONE-WAY. A parent's motion drives its child and the
 * child's reaction does not exist: an arm swing cannot counter-rotate the
 * trunk, and a head flung by a torso stop does not push back on the torso.
 *
 * **THE GAP HAS A STANDARD NAME.** Featherstone's Articulated Body Algorithm --
 * what every ragdoll runs underneath (PhysX, Bullet, Havok, ODE) and what
 * OpenSim and AnyBody use with real muscle models -- has an OUTWARD pass
 * (velocities and accelerations propagate parent -> child) and an INWARD pass
 * (forces propagate child -> parent). We had implemented the outward pass and
 * not the inward one. That is not an approximation invented here; it is half
 * of a standard algorithm.
 *
 * The error it left was SIZED before it was fixed, from our own segment table:
 * head 5.175 kg with its CoM 0.1231 m above the atlas, trunk 32.625 kg with
 * I = 2.81270 kg*m^2 about the hip, neck lever 0.504 m. Per 1 rad/s^2 of head
 * angular acceleration the reaction on the trunk is **0.321 N*m**, perturbing
 * the trunk's own acceleration by **11.4 %** of the head's.
 *
 * ==================== HOW IT IS DONE, AND WHY NOT ABA ====================
 *
 * ABA is O(n) and exists because a games ragdoll has thirty bodies and a
 * millisecond. This body has FOUR joints and the whole model is already
 * LINEARISED in the joint angles (twbodyjoint.h's limit 3), so the honest
 * implementation is the one that is exact for the model we actually have:
 * assemble the linearised mass, damping and stiffness matrices in ABSOLUTE
 * joint angles and integrate the coupled system exactly. Same physics as ABA's
 * two passes for a linear tree, one page of code instead of six, and -- the
 * part that matters -- it REDUCES to `twBodyJointStep`, which is a gate rather
 * than an argument.
 *
 * **THE REDUCTION IS FOUR CLAIMS AND NOT ONE, and reading body_chain_test
 * section 1's own header before touching any tolerance there is not optional.**
 * A single machine-precision equality is NOT available and that is not a bug in
 * either model: they do not carry the same quantity across a step boundary
 * (`twBodyJointStep` keeps the RELATIVE velocity continuous, this keeps the
 * ABSOLUTE one) and do not make the same assumption inside one (the joint
 * freezes the parent's angle and rate; this carries its quadratic motion). So:
 * with a STATIC parent they are identical to 1e-14; with a moving one they
 * agree to O(dt^3) per step and converge first order free-running; and THIS is
 * the exact one, asserted as subdivision invariance rather than as agreement
 * with any reference implementation.
 *
 * ABSOLUTE angles, not relative, and that choice is load-bearing: gravity is
 * then DIAGONAL (a segment's weight depends on where that segment points and
 * on nothing else) while stiffness and damping, which act on RELATIVE angles,
 * carry the off-diagonals. Relative coordinates put the coupling in both.
 *
 * ========================= WHAT IT STILL DOES NOT DO =====================
 *
 *  - **Per-axis decoupled**, exactly as twbodyjoint.h is: three independent
 *    planar problems, no gyroscopic cross-terms BETWEEN axes.
 *  - **Linearised**, so no Coriolis beyond the centripetal stiffening the
 *    single-joint model already carried, and no large-angle geometry.
 *  - **The parent's motion within a step is held constant** for a PRESCRIBED
 *    joint -- the same assumption twBodyJointStep makes, kept deliberately so
 *    the equivalence gate means something.
 *  - **Torque-actuator muscles**, per proposal 44 section 6's standing limit.
 *  - **Every constant is still tw_body's UNSOURCED anthropometry** (AC2.1).
 */

/** Fixed cap: this body has four joints and the solve is O(n^3) in a 2n+1
 * matrix exponential. Eight leaves room without an allocation. */
constexpr int twBodyChainMaxJoints = 8;

struct twBodyChainJoint {
    twBodyJoint joint;

    /** Index of the parent joint, or -1 for a joint whose parent is the GROUND
     * (fixed at zero angle, zero rate, zero acceleration). Must be < this
     * joint's own index: the chain is stored parent-first, which is what makes
     * the ancestor walk a loop rather than a search. */
    int parent = -1;

    /**
     * TRUE when this joint's motion is IMPOSED by the caller rather than
     * solved -- its row leaves the unknowns and its known angle, rate and
     * acceleration move to the right-hand side.
     *
     * This is what makes the equivalence gate possible: a two-joint chain with
     * the root prescribed IS `twBodyJointStep` with a `twBodyParentMotion`, and
     * body_chain_test asserts the two agree to 1e-10 over a full trajectory in
     * every regime. Releasing the prescription is then exactly the inward pass
     * and nothing else, so the difference between the two runs is the reaction
     * and cannot be anything else.
     */
    bool prescribed = false;
};

struct twBodyChain {
    twBodyChainJoint joints[twBodyChainMaxJoints];
    int n = 0;
};

struct twBodyChainState {
    /** ABSOLUTE angle from the segment's own gravity-neutral direction, rad --
     * NOT the angle relative to the parent. `twBodyChainRelative` is the
     * relative one, and it is the quantity a joint limit and a drawn pose want. */
    double angle[twBodyChainMaxJoints][(int) twBodyAxis::Count] = {};
    double vel  [twBodyChainMaxJoints][(int) twBodyAxis::Count] = {};
    /** The acceleration the last step solved for, rad/s^2. For a PRESCRIBED
     * joint this is an INPUT and the caller owns it; for a solved joint it is
     * an output, and it is what a reaction measurement reads. */
    double acc  [twBodyChainMaxJoints][(int) twBodyAxis::Count] = {};
    uint32_t limitHits[twBodyChainMaxJoints] = {};
};

/** The joint's angle relative to its parent, rad -- what `twBodyJointState`
 * stores, what a range-of-motion limit bounds, and what a pose payload draws. */
double twBodyChainRelative( const twBodyChain &c, const twBodyChainState &st,
                            int joint, twBodyAxis a );
double twBodyChainRelativeVel( const twBodyChain &c, const twBodyChainState &st,
                               int joint, twBodyAxis a );

/**
 * The linearised mass matrix about `a`, in ABSOLUTE joint coordinates,
 * row-major n*n. Symmetric.
 *
 * Exposed because it is where the inward pass LIVES and a gate should read it
 * directly rather than infer it from an excursion -- proposal 44 has been
 * caught twice by a bound that a wrong coefficient satisfies comfortably.
 * Two closed forms hold whatever the anthropometry turns out to be:
 *
 *   M[p][c] = m_c * (sigma_p * L_c) * (sigma_c * d_c)     the reaction, SIGNED
 *   M[p][p] = I_prox,p + sum over descendants of m * L^2  the carried mass
 *
 * The second is the term the one-way model omitted ENTIRELY: a trunk that does
 * not know it is carrying two arms and a head is lighter than a trunk, and its
 * natural frequency is correspondingly too high.
 *
 * About the AXIAL axis it is DIAGONAL, and that is geometry rather than a
 * simplification: a segment twisting about its own long axis moves neither its
 * own CoM nor its children's joints sideways, so there is nothing to couple.
 */
void twBodyChainMassMatrix( const twBodyChain &c, twBodyAxis a, double *M );

/**
 * One step of `dt` seconds for the whole chain, per free axis.
 *
 * `activeTorque` is `n * twBodyAxis::Count` doubles in joint-major order, or
 * nullptr. Each entry acts on its joint's RELATIVE angle -- so it appears
 * POSITIVE on that joint's row and NEGATIVE on its parent's, which IS the
 * inward pass for a muscle torque and is the reason an arm swing can
 * counter-rotate a trunk here and could not before.
 *
 * A PRESCRIBED joint's `angle`, `vel` and `acc` are read and never written.
 * A CONSTRAINED axis (outside `freeAxes`) is held at zero, as twBodyJointStep
 * holds it -- zero, not "very stiff".
 *
 * Integration is the EXACT solution of the linear system for coefficients held
 * constant across the step -- a matrix exponential of the augmented state-space
 * form, so the affine forcing is exact too and a singular `A` is not a special
 * case. Proposal 40 section 11.5's forward-Euler lesson is a standing house
 * rule; the 1-DOF reduction of this is `exactStep` to machine precision, which
 * body_chain_test asserts rather than assumes.
 */
void twBodyChainStep( const twBodyChain &c, twBodyChainState &st,
                      const double *activeTorque, double dt );

#endif
