#ifndef _TWBODYOBJECTIVE_H_
#define _TWBODYOBJECTIVE_H_

#include <cstdint>

#include "tw/body/twbodyjoint.h"

/**
 * Proposal 44 C4b -- **THE OBJECTIVE**: what the body is trying to maximise, in
 * numbers. Pure, deterministic: no engine state, no I/O, no Qt, no threads.
 *
 * ============================== WHOSE CLAIM =============================
 *
 * This module implements ONE SENTENCE, and it is the requester's, not a
 * modelling convenience (2026-08-30):
 *
 *   *match is enabling the body to express its urge to move as intensely as it
 *   likes to, over an extended period of time -- tens of seconds*
 *
 * together with the standing assumption the whole of proposal 44 is allowed:
 *
 *   *a match of ongoing movement with trigger is satisfying, and the brain
 *   maximises this satisfaction incrementally.*
 *
 * Three words in there are load-bearing and each becomes a decision below:
 * **INTENSELY** (the reward is achieved movement, NOT alignment -- there is no
 * phase term anywhere in this file), **AS IT LIKES TO** (there is a ceiling,
 * set by the music; exceeding it is not extra reward), and **OVER AN EXTENDED
 * PERIOD** (effort is a BUDGET across a long window, not a per-instant penalty
 * -- which is what makes sustainment the thing being optimised).
 *
 * ==================== WHY EVERY NUMBER HERE IS A RATIO ===================
 *
 * Proposal 44 section 8 question 3 was settled as TORQUE drive precisely so the
 * model would not rest on anything body- or species-specific: a posture is
 * expressed in radians and needs a human-shaped body to mean anything, an
 * activation does not. **That decision is void if the OBJECTIVE smuggles a
 * body-specific unit back in**, so:
 *
 *   - movement is measured in RANGE-LENGTHS PER SECOND -- angular travel
 *     divided by the joint's OWN range of motion. A mouse and a person can both
 *     do "three range-lengths a second";
 *   - effort is measured in units of the segment's OWN gravity moment m*g*d,
 *     so "effort 1" means "sustaining a torque equal to your own weight
 *     moment" whatever you weigh;
 *   - urge is the music's excitation relative to THAT TRACK'S OWN maximum,
 *     times one body constant.
 *
 * ================== THE TWO FREE PARAMETERS, DECLARED ===================
 *
 * There are exactly two, they are PHYSIOLOGICAL rather than taste, and both are
 * stated as anchors rather than as numbers so that a reader can disagree with
 * them precisely:
 *
 *   `maxUrge`      = 4.0 range-lengths/s = **exactly full range, once a
 *                    second** (see twBodyAchievedRate's closed form). How hard
 *                    this body will ever want to go.
 *   `effortBudget` = 1.0 = **sustaining a torque equal to the segment's own
 *                    gravity moment**. How hard it can keep going.
 *
 * Both are VERIFY. Only one of them BINDS at a time -- the budget when the body
 * cannot keep up with the music, the urge when the music is not asking much --
 * and WHICH ONE binds is itself an observable rather than a setting.
 * =========================================================================
 */

/** One hop of one joint's history, as the objective sees it. */
struct twBodyMotionSample {
    double angVel         = 0.0;   // rad/s, the joint's own rate
    double activeTorque   = 0.0;   // N m, the ensemble's contribution
    double posturalTorque = 0.0;   // N m, twBodyPosturalTorque()
    /**
     * The music's excitation at this hop, RELATIVE TO THIS TRACK'S OWN
     * maximum: 0..1, no units, no free parameter. Supplied by the analysis
     * (C5); this module never computes it, because deciding which ensemble
     * quantity means "urge" is a modelling claim and belongs where it can be
     * argued with, not buried in an accumulator.
     */
    double urgeNorm       = 0.0;
};

struct twBodyObjectiveParams {
    /**
     * The window the objective is evaluated over. The requester says tens of
     * seconds; 20 is DERIVED rather than picked -- it is five periods of the
     * slowest unit in proposal 40's ensemble (twobar, 0.25 Hz, a 4 s period),
     * which is the shortest window in which that unit's contribution is a
     * sustained fact rather than a single event.
     */
    double windowSec    = 20.0;

    /**
     * The resolution at which SUSTAINMENT is judged. Without it "over an
     * extended period" has no force: a body that thrashes for five seconds and
     * stops for fifteen has the same window MEAN as one going steadily, and
     * the requester's sentence is plainly about the second. Scoring each
     * sub-window against the urge in that sub-window and summing is what makes
     * a gap cost. Should be about one bar or one drive period.
     */
    double subWindowSec = 1.0;

    /** FREE PARAMETER 1, physiological. Range-lengths per second at full urge.
     * 4.0 is exactly "full range of motion, once a second". VERIFY. */
    double maxUrge      = 4.0;

    /** FREE PARAMETER 2, physiological. Mean effort the body can SUSTAIN across
     * the window, in units of (segment gravity moment)^2. 1.0 is exactly
     * "holding a torque equal to your own weight moment". VERIFY. */
    double effortBudget = 1.0;
};

struct twBodyObjectiveResult {
    double achieved     = 0.0;  // range-lengths/s, mean over the window
    double urge         = 0.0;  // range-lengths/s, mean over the window
    /** [0,1]. sum over sub-windows of min(achieved_i, urge_i), over sum of
     * urge_i. 1 = the body met the urge in every sub-window. A gap anywhere
     * costs, which is the whole point of scoring sub-windows. */
    double match        = 0.0;
    double effort       = 0.0;  // dimensionless, mean over the window
    bool   withinBudget = false;
    /** The WORST sub-window's achieved/urge. `match` says how much was
     * delivered overall; this says whether it was delivered EVENLY, which is
     * the difference between dancing for thirty seconds and twice for five. */
    double weakestSub   = 0.0;
    /** Which ceiling actually bound. Not a setting -- an observation, and the
     * one that says whether a body was held back by the music or by itself. */
    bool   budgetBound  = false;
    int    subWindows   = 0;
};

/**
 * ACHIEVED MOVEMENT, in RANGE-LENGTHS PER SECOND: the joint's mean angular
 * speed divided by its own range of motion.
 *
 * **Angular TRAVEL, not excursion, and that is a choice.** The requester's word
 * is "intensely", and a big slow sway is not the same intensity as a sharp fast
 * one at the same amplitude -- "hard and sudden" was the whole of the Enter
 * Sandman description. Mean |dtheta/dt| carries both amplitude and rate; peak
 * excursion carries only amplitude.
 *
 * **Closed form, which is what makes it gateable:** a sinusoid of amplitude A
 * at f Hz gives mean |dtheta/dt| = (2/pi)*A*omega, so
 *
 *     achieved = 4 * f * ( A / romRad )      range-lengths per second
 *
 * -- full range once a second is exactly 4.0, which is where maxUrge's anchor
 * comes from.
 *
 * NO PHASE TERM, deliberately. Alignment with the beat is not measured here and
 * must not be: the reward is what the body GOT, and resonance is expected to
 * emerge from the budget rather than to be rewarded directly. Building
 * alignment into the reward would make AC4b.2 circular.
 */
double twBodyAchievedRate( const twBodyMotionSample *samples, int count,
                           double dt, double romRad );

/**
 * EFFORT, dimensionless: the mean squared TOTAL muscle torque in units of the
 * segment's own gravity moment.
 *
 * **The postural torque is IN HERE**, and leaving it out is the mistake this
 * signature exists to prevent: an effort term counting only the active torque
 * reports a body straining against a beat as cheaper than one merely standing
 * leant over. It is why twBodyPosturalTorque() is public.
 *
 * SQUARED, per the adversarial review of 2026-08-30: squared torque and
 * non-recoverable work both support the resonance conclusion, while net
 * mechanical work with perfect elastic recovery does not (its mean power is
 * independent of stiffness). Squared torque is the one of the three that is
 * also a standard metabolic proxy, and it is one line.
 */
double twBodyEffort( const twBodyMotionSample *samples, int count,
                     double gravityMoment );

/**
 * The whole objective over one window, for ONE joint.
 *
 * `romRad` is the joint's range of motion about the axis in question (its
 * `limitRad`, or an anatomical range where no hard stop is modelled) and
 * `gravityMoment` is `m*g*d` for its segment. Both come from the joint; they
 * are passed rather than derived so that a caller can evaluate a hypothetical.
 *
 * A window with no urge in it at all scores match 0 and budgetBound false --
 * silence is not a performance, and neither is it a failure.
 */
twBodyObjectiveResult twBodyObjective( const twBodyMotionSample *samples,
                                       int count, double dt, double romRad,
                                       double gravityMoment,
                                       const twBodyObjectiveParams &p );

/**
 * The SCALAR the incremental search climbs: match, minus whatever the budget
 * was overrun by.
 *
 * `lambda` is the budget constraint's LAGRANGE MULTIPLIER, not a taste
 * parameter -- which is the substantive difference between this and the
 * "maximise reward minus lambda times effort" form C4b was first written with.
 * There, lambda decided how much the body cared about effort and had to be
 * tuned. Here it only decides how hard the constraint is enforced, and the
 * OPTIMUM IS INDEPENDENT OF IT as long as it is large enough to bind -- which
 * is a property a gate can check by sweeping it.
 */
double twBodyObjectiveScore( const twBodyObjectiveResult &r,
                             const twBodyObjectiveParams &p,
                             double lambda = 4.0 );

#endif
