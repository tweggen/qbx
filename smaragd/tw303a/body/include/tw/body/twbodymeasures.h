#ifndef _TWBODYMEASURES_H_
#define _TWBODYMEASURES_H_

#include <cstdint>

/**
 * Proposal 44 C2 -- THE MEASURES. Total body mass M and stature H to per-segment
 * mass, length, centre-of-mass position and moment of inertia, plus the two
 * closed forms the plan is built on.
 *
 * Pure, deterministic, single-threaded: no engine state, no I/O, no Qt, no
 * threads, no rand(). The sidecar rules, for the same reason.
 *
 * WHY THIS MODULE EXISTS. Proposal 44 section 1: the ensemble has no masses and
 * no limb lengths, so "how far" is a display constant and the counter-tension
 * is a narration -- without a mass held against gravity there is no
 * Newton-metre behind the number. This is the smallest thing that changes that,
 * and it pays for itself before any ODE is integrated: twBodyCrossoverHz() alone
 * says which metrical level a body part may physically occupy.
 *
 * ============================ READ THIS FIRST ============================
 *
 * **EVERY CONSTANT BELOW IS UNSOURCED AND CARRIES A `VERIFY` MARKER.**
 *
 * The whole point of C2's AC2.1 was to replace remembered numbers with values
 * checked against primary literature, with the citation written beside the
 * constant. **That gate is NOT met.** The environment this was built in has no
 * outbound web fetch -- de Leva (1996) via three hosts, Winter's anthropometry
 * chapter, an NCBI/PMC article and Wikipedia were each refused by the egress
 * proxy. Search returns model-written SUMMARIES of those pages, and quoting a
 * number out of a summary is laundering a memory, not verifying it.
 *
 * That distinction is not pedantry here. Proposal 44's own arm-frequency range
 * (0.9-1.1 Hz) was a remembered number, and the closed form computed from these
 * very constants gives **0.7346 Hz** for the segment section 4 actually defines
 * (upper arm + forearm + hand). Writing that range into a build gate would have
 * made the milestone unresolvable. One remembered number has already cost that;
 * these are the same class of number.
 *
 * So the discipline here is: the STRUCTURE, the closed forms and the tests are
 * real and gate the arithmetic; the CONSTANTS are provisional and say so. The
 * tests below deliberately assert INTERNAL CONSISTENCY and REGRESSION, never
 * agreement with a literature range, because there is no verified range to
 * assert against yet. Whoever has journal access finishes AC2.1 by replacing
 * each `VERIFY` comment with an author/year/table citation and re-running --
 * a value that moves will move a test, which is the point.
 * =========================================================================
 */

/** One segment's inertial description, all in SI. */
struct twBodySegment {
    double mass          = 0.0;   // kg
    double length        = 0.0;   // m
    double comFromProx   = 0.0;   // m, along the segment from its proximal end
    double radiusGyrCom  = 0.0;   // m, about the segment's own CoM
    /** About the PROXIMAL joint, by the parallel-axis theorem:
     *  I = m*(r_gyr^2 + d^2). This is the quantity a hinged-segment torque
     *  balance wants, which is why it is stored rather than re-derived. */
    double inertiaProx   = 0.0;   // kg m^2
};

/** Which segment. Ordered proximal-to-distal within a limb. */
enum class twBodySeg : uint8_t {
    HeadNeck = 0, Trunk, UpperArm, Forearm, Hand, Thigh, Shank, Foot, Count
};

/**
 * The anthropometric model. Both parameters are USER-FACING (proposal 44
 * section 3: "the correlate-with-my-body experiment becomes literal").
 */
struct twBodyMeasures {
    double massKg     = 75.0;    // M
    double statureM   = 1.75;    // H

    /** One segment, scaled from M and H. */
    twBodySegment segment( twBodySeg s ) const;

    /**
     * A COMPOUND pendulum built from consecutive segments, hinged at the first
     * one's proximal joint -- the arm as upper arm + forearm + hand, say.
     * `count` segments starting at `first`, taken in the enum's own order.
     *
     * This exists because "the arm" is genuinely ambiguous and the ambiguity
     * has already cost this proposal one unresolvable gate: the FULL arm and
     * the UPPER ARM ALONE differ by 40% in natural frequency. A caller has to
     * say which it means.
     */
    twBodySegment compound( twBodySeg first, int count ) const;
};

/**
 * The natural frequency of a compound pendulum about its proximal joint,
 * f = sqrt( m*g*d / I_prox ) / 2pi, in Hz. Small-angle.
 */
double twBodyPendulumHz( const twBodySegment &seg );

/**
 * THE CROSSOVER: the frequency at which a rotating segment's INERTIAL demand
 * equals its own gravity moment, at excursion amplitude `amplitudeRad`:
 *
 *     I*theta*w^2 == m*g*d*sin(theta)   =>   f = sqrt( m*g*d*sin(t)/(I*t) )/2pi
 *
 * BELOW it a part is CARRIED -- muscles working against its own weight. ABOVE
 * it the motion is inertial and the weight is beside the point. It is the most
 * actionable number this module produces, because it says which metrical level
 * a body part may physically occupy, and it needs nothing but the measures and
 * an amplitude.
 *
 * It DEPENDS ON AMPLITUDE, through sin(theta)/theta -- a fact worth stating
 * because the plan's first draft quoted crossover values with no amplitude
 * beside them, which makes them unreproducible.
 */
double twBodyCrossoverHz( const twBodySegment &seg, double amplitudeRad );

/** Standard gravity, m/s^2. The one constant here that is not VERIFY-flagged. */
constexpr double twBodyGravity = 9.80665;

#endif
