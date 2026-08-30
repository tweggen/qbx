#ifndef _TWBODYJOINT_H_
#define _TWBODYJOINT_H_

#include <cstdint>

#include "tw/body/twbodymeasures.h"

/**
 * Proposal 44 C4 (first half) -- A JOINT WITH INERTIA.
 *
 * Pure, deterministic: no engine state, no I/O, no Qt, no threads, no rand().
 *
 * ============================ WHY THIS EXISTS ============================
 *
 * The requester's test, which is the clearest statement of the defect anyone
 * has produced in this proposal: *move the torso forward, then STOP. The head
 * still has momentum and keeps going, producing a forward nod, constrained by
 * the atlas joint.* The shipped model cannot show that, and measurement says
 * exactly why:
 *
 *   the neck was a FIRST-ORDER LAG, and a first-order system has no momentum.
 *
 * Simulated over that scenario, the shipped lag's head-relative angle was
 * <= 0 for **6001 of 6001 samples** -- maximum forward overshoot +0.000
 * degrees, at any parameter values. It can only trail its input and settle
 * monotonically. A lag is a FILTER; what the requester describes is a MASS.
 * Overshoot is strictly second order: it needs the segment's own inertia in
 * the equation, which is the `m*d*a_base` term proposal 44 named as missing.
 *
 * The same rule applies at every joint, which is what this module generalises:
 * each joint carries the segment below it, is driven by its parent's
 * ACCELERATION, and differs only in WHICH AXES IT MAY TURN ABOUT. A neck is
 * near-spherical (three axes); a knee or elbow is a hinge (one); a trunk is
 * limited in all three. That distinction is the `freeAxes` mask, and a
 * constrained axis is not merely stiff -- it does not move at all.
 *
 * ===================== WHAT THIS FIRST BUILD DOES NOT DO =================
 *
 *  - **No child-to-parent reaction.** A parent's acceleration drives its
 *    child; the child's reaction torque does NOT yet act back on the parent.
 *    So an arm swing cannot yet counter-rotate the trunk, and proposal 44's
 *    counterswing gate (its AC4.2) is NOT met by this module. That is the
 *    remaining half of C4 and it is deliberately not faked here.
 *  - **Per-axis decoupled.** Each free axis integrates its own second-order
 *    equation. There are no gyroscopic or centrifugal cross-terms, so a joint
 *    turning fast about two axes at once is approximated, not solved.
 *  - **Torque-actuator muscles**, per proposal 44 section 6's standing limit.
 *  - **Every stiffness rests on tw_body's UNSOURCED constants** -- see
 *    twbodymeasures.cc. AC2.1 is still open, so a natural frequency here is
 *    arithmetic over provisional data, not a validated biomechanical number.
 * =========================================================================
 */

/** Which anatomical axis. Matches the skeleton's naming and its axes:
 *  Flex about x (mediolateral), Lateral about z (anteroposterior),
 *  Axial about y (longitudinal). */
enum class twBodyAxis : uint8_t { Flex = 0, Lateral = 1, Axial = 2, Count = 3 };

inline constexpr uint8_t twBodyAxisBit( twBodyAxis a )
{ return (uint8_t) ( 1u << (uint8_t) a ); }

/** Every axis free -- a spherical joint, as the neck approximately is. */
inline constexpr uint8_t twBodySpherical()
{ return twBodyAxisBit( twBodyAxis::Flex ) | twBodyAxisBit( twBodyAxis::Lateral )
       | twBodyAxisBit( twBodyAxis::Axial ); }

/** One axis free -- a hinge, as a knee or an elbow is. */
inline constexpr uint8_t twBodyHinge( twBodyAxis a ) { return twBodyAxisBit( a ); }

struct twBodyJoint {
    /** The segment hanging below this joint, from twBodyMeasures. */
    twBodySegment segment;

    /** Which axes may turn. An axis outside this mask NEVER moves, whatever
     * drives it -- that is what makes a hinge a hinge rather than a stiff
     * ball, and it is asserted rather than assumed. */
    uint8_t freeAxes = twBodySpherical();

    /**
     * Joint stiffness, in units of this segment's own gravity moment m*g*d.
     * Not the total: gravity adds to it or fights it depending on which side
     * of the joint the mass sits (see invertedPendulum).
     *
     * ---------------------------------------------------------------------
     * **THIS IS A CONSTANT ONLY BECAUSE C4's FIRST BUILD MAKES IT ONE. It is
     * not a property of the joint, and it must not be sourced as if it were.**
     *
     * The requester's framing, and it is the right one: a dancing body's joint
     * stiffness is dominated by ACTIVE MUSCLE CO-CONTRACTION, not by passive
     * tissue. It is driven by the rhythmic excitation the sound produces, and
     * it is EMERGENT -- the body settles on the least muscle action that still
     * does the job. So stiffness here is a control variable modulated per hop
     * by the same excitation that drives the motion, and a constant is a
     * placeholder for it.
     *
     * Two consequences worth knowing before anyone "verifies" this number:
     *
     *  - Looking up passive joint stiffness in the biomechanics literature
     *    would give a number for a RELAXED joint, which is the wrong regime.
     *    AC2.1's sourcing task does not apply to this field the way it applies
     *    to a mass or a limb length.
     *  - If stiffness tracks the drive, so does each joint's natural
     *    frequency, and a body that minimises muscle effort has a reason to
     *    tune omega_n TOWARD the rate driving it -- a driven oscillator needs
     *    least torque for a given amplitude at resonance. That would make
     *    resonance with the music an EMERGENT property of impedance
     *    modulation rather than something seeded into an ensemble, which is a
     *    materially different claim from the one proposal 40 makes. It is
     *    also testable: the settled natural frequency should land near the
     *    metrical rate that drives each joint. See C4b in the execution plan.
     * ---------------------------------------------------------------------
     *
     * body_joint_test prints a sweep of what different values imply, so an
     * interim choice is made from data rather than guessed.
     */
    double stiffnessScale = 2.0;

    /**
     * TRUE when the segment's mass sits ABOVE this joint, so gravity is
     * DESTABILISING rather than restoring -- an inverted pendulum. The head on
     * the atlanto-occipital joint is the case in point, and the trunk on the
     * hip is another; an arm hanging from a shoulder is NOT.
     *
     * This is a sign, and getting it wrong is not a detail. With gravity
     * treated as restoring, an upright head appears stable with no muscle tone
     * at all, which is the opposite of true: an inverted segment is stable ONLY
     * where passive tissue exceeds its own gravity moment, i.e. only for
     * stiffnessScale > 1. That threshold is a real, checkable consequence and
     * body_joint_test asserts both sides of it.
     */
    bool invertedPendulum = false;

    /** Damping ratio. Below 1 the joint OVERSHOOTS, which is the whole point;
     * at or above 1 it cannot, and the tests assert both sides. */
    double dampingRatio = 0.30;

    /** Hard range of motion per axis, radians. 0 = unlimited. A joint at its
     * limit stops dead: the outward velocity is killed, not reflected, because
     * ligament stops are dissipative rather than springy. */
    double limitRad[(int) twBodyAxis::Count] = { 0.0, 0.0, 0.0 };

    /** Distance from the PARENT joint to this one, metres. It is the lever
     * through which a parent's angular acceleration becomes linear
     * acceleration at this joint -- the `a_base` in `m*d*a_base`. */
    double parentLever = 0.0;
};

/** The joint's TOTAL stiffness about any axis, N*m/rad: passive tissue, plus
 * or minus this segment's own gravity moment per invertedPendulum. Zero or
 * negative means the joint cannot hold itself up. */
double twBodyJointStiffness( const twBodyJoint &j );

struct twBodyJointState {
    double angle[(int) twBodyAxis::Count] = { 0.0, 0.0, 0.0 };   // rad, vs parent
    double vel  [(int) twBodyAxis::Count] = { 0.0, 0.0, 0.0 };   // rad/s
    uint32_t limitHits = 0;   // how often a hard stop was reached
};

/** True if this joint may turn about `a`. */
bool twBodyAxisIsFree( const twBodyJoint &j, twBodyAxis a );

/** The joint's undamped natural frequency about `a`, Hz. 0 for a constrained
 * axis or a degenerate segment. */
double twBodyJointNaturalHz( const twBodyJoint &j, twBodyAxis a );

/**
 * One step of `dt` seconds.
 *
 * `parentAngAcc` is the PARENT segment's angular acceleration about each axis,
 * rad/s^2 -- the only driving term, and the one the shipped first-order lag
 * had no way to feel. `muscleTorque` is an optional active torque per axis,
 * N*m, for a caller that has one; pass nullptr for a purely passive joint.
 *
 * Integration is the EXACT solution of the linear second-order system for a
 * forcing held constant across the step, not a forward-Euler approximation.
 * Proposal 40 section 11.5's forward-Euler lesson is a standing house rule and
 * twgroovependulum.cc records what it cost there: an integrator whose own
 * per-step gain exceeded 1 injected unbounded growth independent of any real
 * drive. Exactness here costs one exp() and two trig calls per axis.
 */
void twBodyJointStep( const twBodyJoint &j, twBodyJointState &st,
                      const double parentAngAcc[(int) twBodyAxis::Count],
                      const double *muscleTorque, double dt );

/** Mechanical energy about `a`: 0.5*I*w^2 + 0.5*k*theta^2, joules. The
 * conservation gate reads this. */
double twBodyJointEnergy( const twBodyJoint &j, const twBodyJointState &st,
                          twBodyAxis a );

#endif
