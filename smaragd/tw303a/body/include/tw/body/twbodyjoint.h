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
 *    equation, with its OWN inertia, stiffness and drive (see twBodyAxis) --
 *    but there are no gyroscopic cross-terms BETWEEN axes, so a joint turning
 *    fast about two axes at once is approximated, not solved.
 *  - **Linearised in the joint angle.** Gravity enters as `m*g*d*theta`, not
 *    `m*g*d*sin(theta)`, which is what lets the step below be a closed form
 *    rather than a numerical integration. The linear model OVERSTATES a large
 *    droop: at the stiffnesses this module defaults to, a 25 degree trunk lean
 *    settles the head 25 degrees relative where the nonlinear equation gives
 *    about 20. Exactness of the INTEGRATOR is not exactness of the MODEL and
 *    the tests say which is which.
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
     * ABOUT THE LONG AXIS THIS SCALE IS BORROWED, and that is a modelling
     * choice rather than physics. Twisting a segment neither raises nor lowers
     * its CoM, so gravity supplies no moment about that axis and therefore no
     * natural unit to express a stiffness in. Using m*g*d there anyway keeps
     * axial stiffness the same ORDER as sagittal without inventing a second
     * unsourced constant; what it does NOT do is add or subtract a gravity
     * term, so an axial axis has stiffness `stiffnessScale * m*g*d` flat, no
     * inverted-pendulum threshold, and no crossover.
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

    /**
     * Damping ratio, referred to the joint's REFERENCE stiffness (see
     * twBodyJointDamping) rather than to whatever stiffness the joint has at
     * this instant. Below 1 the joint rings; above it, free decay is monotone.
     *
     * **"Overdamped cannot overshoot" is only true of FREE DECAY, and this
     * header used to state it as an absolute.** Under a bipolar drive --
     * accelerate, then decelerate, which is exactly the requester's torso stop
     * -- an overdamped joint crosses its equilibrium perfectly happily;
     * body_joint_test's own zeta = 1.4 run does. What damping above 1 buys is
     * *far less* overshoot, and that is what the test asserts.
     *
     * THE RATIO IS A CONVENIENCE, NOT THE STORED QUANTITY. What the equation
     * uses is an absolute damping COEFFICIENT c in N*m*s/rad, computed once
     * from this ratio and the reference stiffness. That indirection is load
     * bearing: a ratio ties c to sqrt(k), so a joint whose stiffness is
     * modulated -- by the centripetal term below, or per hop once C4b makes
     * co-contraction a control variable -- would silently modulate its own
     * damping too, and c would VANISH as an inverted joint approached its
     * stability threshold. Muscle and tissue viscosity does neither.
     */
    double dampingRatio = 0.30;

    /**
     * POSTURAL TONE: the fraction of this joint's own gravity moment that
     * muscle is holding, 0..1. **0 = an UNACTIVATED joint** -- passive tissue
     * and nothing else, which is what every other field here describes and
     * therefore the default. 1 = gravity fully cancelled at this joint.
     *
     * WHY THIS EXISTS, and it is proposal 44 section 8 question 3's decision
     * made concrete. The ensemble's output drives the plant as a TORQUE, not
     * as a posture target, so nothing in the musical signal knows which way is
     * down -- and an inverted segment needs a standing torque simply to stay
     * up. Left in one lump, that hold would have to ride inside the musical
     * torque as a large DC offset, which is an opinion about posture smuggled
     * in as a constant. Split out, it is
     *
     *     tau_muscle = tau_postural( body, pose )  +  tau_active( ensemble )
     *
     * and the first term is DERIVED WITH NO FREE PARAMETER: it is whatever
     * cancels m*g*d at this joint, a quantity the measures module already
     * computes. Gravity compensation is a physical requirement, not a style
     * choice, which is exactly what keeps it out of the socialisation
     * objection the whole model is built to answer.
     *
     * **IT IS A GAIN AND NOT A TORQUE, DELIBERATELY.** Postural tone is
     * feedback proportional to the segment's ABSOLUTE angle -- the same
     * quantity gravity itself acts on -- so it belongs in the coefficients,
     * beside the term it cancels, not in the forcing array. Applied as an
     * explicit torque sampled at the top of a step it would leave a residual
     * within the step that grows with dt, and "fully compensated" would not
     * hold still exactly, so nothing could gate it. Here it is exact at any dt
     * by construction: both gravity terms are simply scaled by (1 - gain).
     *
     * TWO CONSEQUENCES WORTH KNOWING BEFORE READING ANY OTHER NUMBER HERE.
     * At gain 1 an inverted joint has NO stability threshold at all -- a live
     * person's head is held up by muscle, not by ligament, so `ks > 1` is a
     * claim about an UNACTIVATED neck and about nothing else. And at gain 1 a
     * hanging joint with no passive tissue no longer hangs plumb: it stays
     * where it is put, which is what "holding your arm out" IS.
     *
     * About the LONG AXIS this is inert, because there is no gravity moment
     * there to compensate (invariant 3).
     */
    double posturalGain = 0.0;

    /** Hard range of motion per axis, radians. 0 = unlimited. A joint at its
     * limit stops dead: the outward velocity is killed, not reflected, because
     * ligament stops are dissipative rather than springy. */
    double limitRad[(int) twBodyAxis::Count] = { 0.0, 0.0, 0.0 };

    /** Distance from the PARENT joint to this one, metres. It is the lever
     * through which a parent's angular acceleration becomes linear
     * acceleration at this joint -- the `a_base` in `m*d*a_base`. */
    double parentLever = 0.0;
};

/**
 * The joint's TOTAL stiffness about `a`, N*m/rad -- passive tissue, plus or
 * minus the UNCOMPENSATED part of this segment's gravity moment (see
 * posturalGain) per invertedPendulum. Zero or negative means the joint cannot
 * hold itself up about that axis WITHOUT MUSCLE, which is a different claim
 * from "cannot hold itself up".
 *
 * PER AXIS, and the axis argument is not decoration. About the LONG axis there
 * is no gravity moment at all (see stiffnessScale), so the +/- m*g*d term is
 * absent, an axial axis has no stability threshold, and a joint that is
 * unstable in flexion may be perfectly stable in twist.
 *
 * This is the NOMINAL stiffness. The stiffness the step actually integrates
 * also carries the centripetal term `m*d*L*phiVel^2`, which depends on how
 * fast the parent is turning and so cannot live in a function of the joint
 * alone -- twBodyJointStiffnessAt() is that one.
 */
double twBodyJointStiffness( const twBodyJoint &j, twBodyAxis a );

/** The inertia the joint feels about `a`, kg*m^2: the segment's transverse
 * inertia about the joint for flexion and lateral flexion, its LONGITUDINAL
 * inertia for twist. Those differ by more than an order of magnitude. */
double twBodyJointInertia( const twBodyJoint &j, twBodyAxis a );

/** The absolute damping coefficient about `a`, N*m*s/rad. Computed from
 * dampingRatio at the joint's REFERENCE stiffness -- passive tissue plus the
 * magnitude of the gravity moment, which is positive whichever side the mass
 * sits -- so it is independent of the inverted flag, does not vanish at the
 * stability threshold, and does not move when the stiffness is modulated. */
double twBodyJointDamping( const twBodyJoint &j, twBodyAxis a );

/**
 * THE PARENT'S MOTION, which is the whole input to a joint.
 *
 * All three fields are needed and each buys a different term. The first build
 * of this module passed only the acceleration, and the omission was not a
 * refinement: with no parent ANGLE, gravity can only be applied to the joint's
 * RELATIVE angle, and a freely hanging arm on a leaning trunk then settles
 * PARALLEL TO THE LEANING TRUNK instead of hanging plumb. That is the
 * one-sentence falsifier for the truncated equation, and it is section 3a of
 * body_joint_test.
 */
struct twBodyParentMotion {
    /** The parent segment's ABSOLUTE angle from the vertical, rad, per axis --
     * NOT its angle relative to ITS parent. Gravity acts on where a segment
     * actually points, and the child's absolute angle is parent + relative. */
    double angle[(int) twBodyAxis::Count] = { 0.0, 0.0, 0.0 };
    /** rad/s. Buys the centripetal stiffening `m*d*L*phiVel^2`, which is 2.2
     * N*m/rad at 1 Hz and 35.7 at 4 Hz for a head on a 25-degree trunk sway,
     * against a total stiffness of about 5.8 -- negligible at a slow sway and
     * DOMINANT at the rates this feature is about. It always stiffens, so the
     * effective resonance under a fast drive sits above twBodyJointNaturalHz. */
    double angVel[(int) twBodyAxis::Count] = { 0.0, 0.0, 0.0 };
    /** rad/s^2. The requester's term: the reaction to the parent's own angular
     * deceleration, plus its joint accelerating this one linearly through the
     * lever. This is what a first-order lag has no way to feel. */
    double angAcc[(int) twBodyAxis::Count] = { 0.0, 0.0, 0.0 };
};

/** The stiffness the step integrates about `a`, including the parent's
 * centripetal contribution. Equals twBodyJointStiffness() at rest. */
double twBodyJointStiffnessAt( const twBodyJoint &j, twBodyAxis a,
                               const twBodyParentMotion &p );

struct twBodyJointState {
    double angle[(int) twBodyAxis::Count] = { 0.0, 0.0, 0.0 };   // rad, vs parent
    double vel  [(int) twBodyAxis::Count] = { 0.0, 0.0, 0.0 };   // rad/s
    /** Steps spent pressed against a hard stop -- DWELL, not events. A joint
     * held at its limit for a second at dt = 0.2 ms scores 5000. Useful as
     * "did it ever reach the stop" and as a rough occupancy; never as a count
     * of arrivals. */
    uint32_t limitHits = 0;
};

/** True if this joint may turn about `a`. */
bool twBodyAxisIsFree( const twBodyJoint &j, twBodyAxis a );

/** The joint's undamped natural frequency about `a`, Hz. 0 for a constrained
 * axis, a degenerate segment, or a non-positive stiffness. */
double twBodyJointNaturalHz( const twBodyJoint &j, twBodyAxis a );

/**
 * The joint's EQUILIBRIUM angle about `a` under a given parent motion, rad --
 * where it settles once the ringing has died, which for a driven joint is NOT
 * zero. A hanging arm on a trunk leaning phi settles at -phi (plumb); an
 * inverted head on a trunk leaning phi DROOPS to +phi*mgd/k.
 *
 * Exposed because a gate that measures "has it come to rest" must measure the
 * distance from THIS, not from zero. Measuring from zero is how the first
 * build's arrest test came to assert something the corrected equation cannot
 * satisfy.
 */
double twBodyJointEquilibrium( const twBodyJoint &j, twBodyAxis a,
                               const twBodyParentMotion &p,
                               const double *activeTorque = nullptr );

/**
 * One step of `dt` seconds. The equation, per free axis:
 *
 *     I th'' + c th' + [ k_pass -/+ m g d + m d L phiVel^2 ] th
 *                       = -( I + m d L ) phiAcc  +/-  m g d phi  + tau_muscle
 *
 * with the upper sign for an inverted segment. Reading the four terms right to
 * left: the parent's acceleration drives the joint through its own inertia and
 * through the lever L; gravity pulls on the child's ABSOLUTE angle, which
 * splits into a stiffness term in `th` and a FORCING term in `phi`; the
 * parent's rotation stiffens the joint centripetally.
 *
 * `activeTorque` is the ACTIVE torque per axis, N*m -- for proposal 44, the
 * ensemble's own output and nothing else. **POSTURAL TONE IS NOT IN HERE**:
 * it is `posturalGain`, because it is feedback on the absolute angle and has
 * to sit beside the gravity term it cancels to be exact. nullptr for a joint
 * that is doing nothing beyond holding itself up.
 *
 * Integration is the EXACT solution of that linear system for coefficients
 * held constant across the step, not a forward-Euler approximation, in all
 * four regimes -- underdamped, critical, overdamped, and NEGATIVE STIFFNESS.
 * That last one is the analytic continuation of the overdamped branch and it
 * is the reason an unstable joint actually falls over here: the first build
 * folded k < 0 into "no stiffness", which held a joint that should have
 * diverged as cosh(5.04 t) perfectly still.
 *
 * Proposal 40 section 11.5's forward-Euler lesson is a standing house rule and
 * twgroovependulum.cc records what it cost there: an integrator whose own
 * per-step gain exceeded 1 injected unbounded growth independent of any real
 * drive. Exactness here costs one exp() and two trig calls per axis.
 */
void twBodyJointStep( const twBodyJoint &j, twBodyJointState &st,
                      const twBodyParentMotion &parent,
                      const double *activeTorque, double dt );

/**
 * The POSTURAL torque this joint's muscle is supplying about `a`, N*m -- the
 * cost of holding the segment up in its current pose. Signed, in the joint's
 * own sense, and LINEARISED to match exactly what the step applies (a
 * compensation that cancelled the true sine would not cancel what the equation
 * actually integrates, and the two must agree or nothing can be gated).
 *
 * Exposed because C4b's objective needs it: holding a head up while the trunk
 * leans costs real muscle, and an effort term that counted only the active
 * torque would report a body straining against a beat as cheaper than one
 * standing still. Zero at posturalGain 0, and zero about the long axis.
 */
double twBodyPosturalTorque( const twBodyJoint &j, twBodyAxis a,
                             const twBodyParentMotion &p,
                             const twBodyJointState &st );

/** Mechanical energy about `a`: 0.5*I*w^2 + 0.5*k*theta^2, joules, over the
 * NOMINAL stiffness and the axis's own inertia. The conservation gate reads
 * this. Meaningful only where the stiffness is positive. */
double twBodyJointEnergy( const twBodyJoint &j, const twBodyJointState &st,
                          twBodyAxis a );

#endif
