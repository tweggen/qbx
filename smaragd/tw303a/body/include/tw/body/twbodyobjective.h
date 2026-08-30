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
 *   `maxUrge`      = 12.0 range-lengths/s = **exactly full range, THREE times
 *                    a second** (see twBodyAchievedRate's closed form). How
 *                    hard this body will ever want to go. Requester's value,
 *                    2026-08-30; the anchor is what makes it arguable, and the
 *                    thing to note is that a hard 2 Hz head-bang at full range
 *                    is 8.0, so this leaves the ceiling OFF most of the time
 *                    and lets the budget be what binds.
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
     * The music's excitation at this hop, 0..1. Supplied by the analysis (C5);
     * this module never computes it, because deciding which ensemble quantity
     * means "urge" is a modelling claim and belongs where it can be argued
     * with, not buried in an accumulator.
     *
     * **IT MUST BE AN ABSOLUTE SCALE, COMPARABLE ACROSS TRACKS** (requester,
     * 2026-08-30). The first draft normalised each track against ITS OWN
     * maximum, which is wrong for what this model is FOR: under that rule a
     * gentle ambient piece and a rock build-up both drive the body to its
     * ceiling at their own loudest moment, and the claim that *this* music
     * demands more movement than *that* music becomes inexpressible -- while
     * being exactly the claim proposal 44 exists to test. An absolute
     * reference (full scale, not the track's peak) is what makes a quietly
     * mastered track genuinely offer less.
     *
     * ONE CONSEQUENCE OF THE CHANGE, and it is why this field is clamped
     * rather than trusted: with a per-track normalisation, values above 1 were
     * impossible BY CONSTRUCTION. With an absolute one they are not -- a track
     * louder than the reference produces them. They are clamped to 1 here, and
     * the CEILING is what makes that harmless: past full urge, more urge earns
     * the body nothing anyway.
     */
    double urgeNorm       = 0.0;
};

struct twBodyObjectiveParams {
    /**
     * The window the objective is evaluated over. The requester says tens of
     * seconds, and 24 is DERIVED from two constraints meeting rather than
     * picked: it is SIX periods of the slowest unit in proposal 40's ensemble
     * (twobar, 0.25 Hz, a 4 s period) -- the shortest window in which that
     * unit's contribution is a sustained fact rather than an event -- and it
     * is exactly THREE sub-windows at the 8 s below, so no sub-window is
     * ragged. It was 20 s while the sub-window was 1 s, which divided 20
     * evenly; 8 does not, and a ragged trailing sub-window is scored against a
     * full sub-window's urge, which quietly penalises the end of every run.
     */
    double windowSec    = 24.0;

    /**
     * The resolution at which SUSTAINMENT is judged. Without it "over an
     * extended period" has no force: a body that thrashes for a third of the
     * window and stops has the same window MEAN as one going steadily, and the
     * requester's sentence is plainly about the second. Scoring each
     * sub-window against the urge in that sub-window and summing is what makes
     * a gap cost.
     *
     * **8 s, the requester's value (2026-08-30), and the scale is the claim.**
     * At 120 BPM that is four bars -- a PHRASE, not a bar and not a beat. The
     * consequence is deliberate and is gated as a pair: a dancer who pauses
     * for two seconds inside every eight is scored as sustaining perfectly,
     * while one who sits out a whole phrase is not. Judging at 1 s would score
     * the first at 0.75 and call ordinary phrasing a failure.
     */
    double subWindowSec = 8.0;

    /** FREE PARAMETER 1, physiological. Range-lengths per second at full urge.
     * 12.0 is exactly "full range of motion, THREE times a second". VERIFY. */
    double maxUrge      = 12.0;

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
 * -- full range once a second is exactly 4.0, and three times a second exactly
 * 12.0, which is where maxUrge's anchor comes from.
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
