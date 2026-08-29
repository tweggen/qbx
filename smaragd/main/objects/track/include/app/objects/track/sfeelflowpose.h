#ifndef SFEELFLOWPOSE_H
#define SFEELFLOWPOSE_H

#include "app/objects/track/sfeelflowbounce.h"   // SFeelFlowUiData
#include "tw/core/twtypes.h"                     // offset_t

#include <string>

/**
 * Proposal 40 "Feel Flow" M3e AC 4 -- THE POSE.
 *
 * One PURE FUNCTION of one immutable snapshot. Section 3.4's "per-body-part
 * display" given a body: the pendulum ensemble's own state at a position,
 * mapped onto a 2D stick figure's joints. Read this before touching it --
 * three things are deliberate and each of them is the difference between an
 * honest readout and a plausible-looking animation:
 *
 *  1. **Nothing is computed here that the analysis did not already export.**
 *     The displacement of a part is `sqrt(unitPower) * cosMet` -- the unit's
 *     own amplitude times the cosine of its own phase, both read from the
 *     decoded aspects at the frame's hop. There is no oscillator, no
 *     nominal frequency, no free-running clock: a puppet whose motion is
 *     synthesized from a tempo would move beautifully and mean nothing,
 *     which is the exact failure mode a feature about honest physical
 *     correlation cannot afford. This is the level meters' rule again (read
 *     BY POSITION out of what is already frozen, never recompute at paint
 *     time) and the metronome's (a pure function of the position, so a
 *     seek, a wrap or a repaint can never leave it out of step).
 *
 *  2. **`cosMet` is used RAW -- deliberately UN-renormalized.** The aspect's
 *     cos/sin channels are BIN-AVERAGED separately, which IS the circular
 *     mean's numerator, so `hypot(cosPhi,sinPhi) <= 1` and the shortfall
 *     MEASURES the in-bin phase spread (measured range on the a_offset15
 *     fixture: [0.598, 1]). Dividing by that magnitude to recover a "pure"
 *     phase would invent full-amplitude motion out of a bin whose phase was
 *     incoherent. Left raw, an incoherent bin damps the excursion by
 *     construction -- the display gets quieter exactly where the physics is
 *     less certain, which is the behaviour we want and costs no extra code.
 *
 *  3. **The unit-to-part mapping is BY NAME**, against the Burger/Toiviainen
 *     seeding the default ensemble was built from (design section 3.4), not
 *     by ensemble index: a trained structure can carry a different unit set
 *     in a different order, and a puppet silently driven by whatever
 *     happened to be at index 2 would be worse than one that stands still.
 *     An unknown or absent name leaves that part at 0 -- never a fallback to
 *     a neighbouring unit.
 *
 * The three "no motion" outcomes are deliberately DISTINCT, because a caller
 * has to be able to tell them apart:
 *
 *   - **invalid** (`valid == false`, everything 0): there is no usable
 *     snapshot at all -- no analysis, an empty/short `dyn` (a pre-v2 store),
 *     or a negative frame. The widget draws the neutral figure DIMMED with a
 *     note. STALENESS is deliberately NOT checked here: this function never
 *     sees the track, and the painter's own visibility rule stays the single
 *     place that decision is made (the same split
 *     SFeelFlowTrackBounce::feelFlowForUi() already documents).
 *   - **past the material** (`valid == true`, everything 0): the analysis is
 *     fine, the playhead is simply beyond the last analyzed hop. The puppet
 *     STANDS STILL -- it does not freeze on the last pose (which would read
 *     as "still grooving" over silence) and it does not go invalid (which
 *     would read as "no analysis").
 *   - **a genuinely still moment inside the material**: also all-zero, and
 *     indistinguishable from the case above by design. There is nothing to
 *     distinguish: both mean "this part is not moving right now".
 */
struct SFeelFlowPose {
    // False: no usable snapshot. Everything below is 0 in that case. The
    // CALLER checks staleness separately (see the class doc).
    bool  valid    = false;

    // Every displacement is in [-1,1], clamped defensively -- a snapshot is
    // immutable and normalized, but a widget must never be handed a joint
    // angle it cannot draw.
    float bounceY  = 0.0f;   // pelvis vertical      <- unit "bounce"
    float sway     = 0.0f;   // torso lean           <- unit "sway"
    float armSwing = 0.0f;   // arm swing (antiphase)<- unit "limbs"
    float headNod  = 0.0f;   // head nod             <- unit "reference"
    float hipShift = 0.0f;   // hip x-shift          <- unit "twobar"

    // Per part, in the SAME order as the five fields above: sqrt(unitPower),
    // i.e. the unit's amplitude, in [0,1]. This is the DISPLACEMENT's own
    // envelope (|displacement| <= energy by construction, since |cosPhi| <= 1),
    // which is what makes it usable as a brightness/weight without a second
    // normalization: a part at energy 0 is not participating at all, a part
    // at energy 1 is at its own run peak.
    float energy[5] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };

    // Index into energy[] / the field order above.
    enum Part { PartBounce = 0, PartSway = 1, PartLimbs = 2,
                PartReference = 3, PartTwobar = 4, PartCount = 5 };
};

/**
 * The pose at `frame` (the analyzed-wave frame domain, which is the track
 * timeline's own domain -- see SFeelFlowUiData's doc for why: the bounce
 * always starts at project frame 0).
 *
 * Linearly interpolates BOTH the per-unit power and the per-unit cosPhi
 * between the two adjacent hops (`frame / hopFrames` is fractional), so the
 * motion is smooth at any repaint rate rather than stepping at the aspect's
 * 100 Hz grid. Interpolating the power and taking the square root of the
 * RESULT (rather than interpolating two square roots) keeps the energy a
 * monotone function of the interpolated power and costs one sqrt per part.
 *
 * Never blocks, never demands, never touches the sidecar store, never reads
 * the model: everything it needs is in the snapshot it is given.
 */
SFeelFlowPose sFeelFlowPoseAt( const SFeelFlowUiData &ui, offset_t frame );

/** The describe() grammar both the verb and the widget's own note use --
 * "pose: valid=1 bounceY=... sway=... armSwing=... headNod=... hipShift=...
 * energySum=...", 4 decimals. ONE spelling, so a gate and a paint can never
 * disagree about what a pose says (proposal 41 M7's tagChipRect() lesson).
 * Returns a std::string so app/objects/track needs no Qt string type here. */
std::string sFeelFlowPoseDescribe( const SFeelFlowPose &pose );

#endif // SFEELFLOWPOSE_H
