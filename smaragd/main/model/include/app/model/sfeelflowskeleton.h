#ifndef SFEELFLOWSKELETON_H
#define SFEELFLOWSKELETON_H

#include <QPointF>
#include <QRectF>
#include <QString>

/**
 * Proposal 44 execution plan, C0 -- THE PUPPET'S SKELETON, as ONE pure
 * function.
 *
 * Read this before touching the puppet's geometry: it lives here, in
 * `app/model`, rather than inside `SFeelFlowPuppetWidget::paintEvent`, and
 * that placement is the whole point.
 *
 *  1. **The painter and the gate must not be able to disagree.** The widget
 *     draws what this returns and computes no geometry of its own; the
 *     `assert-puppet-skeleton` verb evaluates this same function. Two
 *     implementations of one layout is exactly how proposal 41 M7's hit-test
 *     drifted from its paint for two whole milestones (`tagChipRect()`), and
 *     how the take lane came to fill its body by a different rule than the
 *     composite lane (`fillBodyByMaterial()`). ONE spelling, both callers.
 *  2. **`app/model` is the lowest app layer**, so `timeline` (the widget) and
 *     `testkit` (the verb) can both reach it -- they already declare `model`
 *     in `check_layering.py`'s APP_DEPS, so this needs no new edge.
 *  3. **It takes plain joint scalars, never `SFeelFlowPose`.** That type lives
 *     in `app/objects/track`, which `app/model` may not include. The same
 *     argument `sclipwindowgeometry.h` makes, and it has a second benefit: the
 *     geometry is testable with no analysis, no fixture and no sidecar at all,
 *     which is what makes C0's gate possible (nothing in this repo can drive
 *     an analysis-derived pose to a chosen angle).
 *
 * **THE DEFECT THIS FIXES.** The widget used to rotate the neck about the
 * pelvis correctly and then build every segment above it in WORLD
 * coordinates from the neck's new position:
 *
 *     headC     = rotateAbout( neck, QPointF( neck.x(), neck.y() - r ), nod );
 *     shoulderL = QPointF( neck.x() - shoulderW, neck.y() + torsoLen*0.06 );
 *
 * `(neck.x, neck.y - r)` is straight up on the SCREEN, not along the torso
 * axis, so the head, the shoulder bar and both arms inherited the neck's
 * TRANSLATION and none of its ROTATION. Measured on the shipped build: at a
 * trunk lean of 20 degrees the head stub and the shoulder bar both read
 * **0.000 degrees** -- a figure with no spine, the head effectively
 * gimbal-mounted. Every segment here is therefore built in TORSO-LOCAL
 * coordinates: its own rotation is composed ON TOP of the trunk's.
 *
 * **What this is NOT.** The head now follows the trunk RIGIDLY -- no neck lag,
 * no counter-rotation, no inertia. That is correct-but-crude and is
 * deliberately all C0 promises; the dynamics are C4's `m*d*a_base` term. The
 * difference this makes is between a wrong drawing and a crude one.
 *
 * Pure: no widget, no model, no clock, no allocation beyond the return value.
 */

/** The five normalized joint excursions, each nominally in [-1,1]. Names match
 * `SFeelFlowPose`'s fields so a caller can copy them across one-for-one; the
 * type is deliberately separate so this header needs no `objects/track`. */
struct SFeelFlowJoints {
    float bounceY  = 0.0f;   // pelvis vertical, + is a LIFT
    float sway     = 0.0f;   // trunk lean
    float armSwing = 0.0f;   // arm swing, drawn in antiphase
    float headNod  = 0.0f;   // head angle RELATIVE TO THE TRUNK
    float hipShift = 0.0f;   // pelvis horizontal
};

/**
 * Every point the puppet draws, plus the world angle of every segment whose
 * orientation is contractual.
 *
 * The angles are computed GEOMETRICALLY, from the returned points, never
 * echoed back from the inputs that produced them. That is what lets a gate on
 * them bite: an assertion against a number this function was told would be
 * tautological, while an assertion against a number recovered from the
 * resulting geometry fails when the construction is wrong.
 *
 * Sign convention, one for the whole struct: degrees from the segment's own
 * REST direction, POSITIVE = clockwise on screen (screen y grows downward).
 * A trunk at +20 leans to the viewer's right.
 */
struct SFeelFlowSkeleton {
    bool valid = false;      // false: the box is too small to lay out

    QPointF pelvis, neck, headCentre, headBase;
    QPointF shoulderL, shoulderR, armEndL, armEndR;
    QPointF footL, footR, hipL, hipR;
    double  headRadius = 0.0;
    double  groundY    = 0.0;

    // Geometric, from the points above. With armSwing and headNod at 0 these
    // four are all EQUAL to trunkLeanDeg -- that identity is what C0 gates.
    double trunkLeanDeg    = 0.0;   // pelvis -> neck, from world up
    double headStubLeanDeg = 0.0;   // neck -> head base, from world up
    double shoulderBarDeg  = 0.0;   // shoulderL -> shoulderR, from horizontal
    double armLeanLDeg     = 0.0;   // shoulderL -> arm end, from world down
    double armLeanRDeg     = 0.0;   // shoulderR -> arm end, from world down
};

/** The full excursion of each joint, in degrees or as a fraction of the box.
 * Display constants: nothing downstream reads them and no gate pins them
 * (aesthetics are explicitly not gated). They live here rather than in the
 * widget so the ONE function that uses them owns them. */
namespace sfeelflowskel {
constexpr double kSwayDeg    = 20.0;
constexpr double kNodDeg     = 10.0;
constexpr double kArmDeg     = 35.0;
constexpr double kBounceFrac = 0.06;
constexpr double kHipFrac    = 0.08;
}

/** The skeleton for `j` laid out inside `box`. `valid` is false, and every
 * point zero, when the box cannot hold a figure. */
SFeelFlowSkeleton sFeelFlowSkeletonFor( const SFeelFlowJoints &j, const QRectF &box );

/** The describe() grammar the verb and any diagnostic share -- ONE spelling,
 * 4 decimals, C locale unconditionally (this box's LC_NUMERIC is de_DE and a
 * decimal COMMA would break every `contains=` downstream, the same trap
 * `sFeelFlowPoseDescribe` documents). */
QString sFeelFlowSkeletonDescribe( const SFeelFlowSkeleton &s );

#endif // SFEELFLOWSKELETON_H
