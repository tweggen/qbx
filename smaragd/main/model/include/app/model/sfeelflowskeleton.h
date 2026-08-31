#ifndef SFEELFLOWSKELETON_H
#define SFEELFLOWSKELETON_H

#include <QPointF>
#include <QRectF>
#include <QString>

#include <vector>

/**
 * Proposal 44 C0 + C3 -- THE PUPPET'S SKELETON: a genuinely THREE-DIMENSIONAL
 * body, and a separate orthographic projection of it to a wireframe.
 *
 * Read this before touching the puppet's geometry.
 *
 * ---------------------------------------------------------------------------
 * WHY 3D, WHEN THE POSE STILL HAS FIVE SCALARS
 *
 * The shipped puppet was drawn front-facing with every DOF a rotation or
 * translation in the single SCREEN plane. That made three of its parts
 * anatomically wrong by construction, and the requester's own stated core
 * interaction -- the trunk bending FORWARD and returning upright -- had no
 * degree of freedom at all, because a front view foreshortens sagittal motion
 * to nothing. A 2D figure cannot be fixed by choosing better numbers; the
 * missing thing is an axis.
 *
 * So the MODEL is 3D and the CAMERA is display. Every joint is a point in body
 * space; every rotation names the anatomical axis it turns about:
 *
 *     x  to the subject's LEFT-RIGHT   (mediolateral)   -> sagittal rotations
 *     y  UP                            (longitudinal)   -> axial rotations
 *     z  toward the VIEWER             (anteroposterior)-> frontal rotations
 *
 * A rotation "about x" is therefore flexion/extension (bending forward and
 * back); "about z" is lateral flexion (leaning toward a shoulder); "about y"
 * is axial rotation (twisting). Those three are distinct motions and the
 * shipped model had only the middle one -- see the verification note's 9.7.
 *
 * ---------------------------------------------------------------------------
 * WHAT THIS BUYS TODAY, AND WHAT IT DOES NOT
 *
 * BUYS: the arm swing moves to the SAGITTAL plane, where an arm swing actually
 * happens. It was drawn as lateral abduction -- a jumping-jack -- which the
 * verification note named and which no amount of 2D drawing could fix. And a
 * three-quarter camera shows depth, so a sagittal DOF is legible the moment
 * one is driven.
 *
 * DOES NOT BUY: a driven trunk flexion. The pose carries five scalars and none
 * of them is a fore-aft trunk command; `trunkFlex` exists in the joint set,
 * defaults to 0, and is wired all the way through the model and the projection
 * so that C3's remaining half is a matter of driving it, not of building it.
 * A reader should not mistake "the axis exists" for "the body bends forward".
 *
 * ---------------------------------------------------------------------------
 * THE TWO STANDING RULES, UNCHANGED FROM C0
 *
 *  1. **ONE function, two callers.** The painter draws what this returns and
 *     computes no geometry; `assert-puppet-skeleton` evaluates this same
 *     function. Two implementations of one layout drift, and they drift
 *     silently -- proposal 41 M7 paid for that with `tagChipRect()` and the
 *     take-lane fix paid for it again with `fillBodyByMaterial()`.
 *  2. **Every segment above the pelvis is built in PARENT-LOCAL coordinates**,
 *     so its own rotation composes ON TOP of its parent's. The shipped bug was
 *     the opposite: the head, shoulder bar and arms were rebuilt in WORLD
 *     coordinates from the neck's new position, so at a 20 degree trunk lean
 *     they all read 0.000 degrees -- a figure with no spine.
 *
 * `app/model` is the lowest app layer, so `timeline` (the widget) and `testkit`
 * (the verb) both reach it with no new edge. It takes plain scalars, never
 * `SFeelFlowPose` (which lives in `app/objects/track` and may not be included
 * here) -- the `sclipwindowgeometry.h` argument, with the same second benefit:
 * the geometry is testable with no analysis, no fixture and no sidecar.
 *
 * Pure: no widget, no model, no clock.
 */

/** A point or direction in BODY space. See the axis convention above. */
struct SFeelFlowVec3 {
    double x = 0.0, y = 0.0, z = 0.0;
};

namespace sfeelflowskel {

/**
 * WHICH ANATOMICAL PLANE the trunk's driven flexion and the head's nod are
 * applied in. **VERIFY, and it is unsourced in BOTH directions.**
 *
 * Requester decision, 2026-08-31: SAGITTAL. Recorded with its provenance
 * because the evidence situation is unusual and a later reader must not
 * mistake this for a sourced value.
 *
 * WHAT THE LITERATURE THIS PROJECT CITES ACTUALLY SAYS: nothing. Proposal 40
 * carries every citation the model rests on -- Toiviainen/Luck/Thompson 2010,
 * Burger 2013, Hove 2014, van Noorden & Moelants 2001 -- and the words
 * "sagittal", "frontal", "lateral", "fore-aft", "anteroposterior" and
 * "mediolateral" appear in it ZERO times. Every one of those citations fixes
 * a metrical LEVEL (Toiviainen), an audio band to a body PART (Burger, Hove)
 * or a TEMPO (van Noorden & Moelants). Not one fixes a PLANE.
 *
 * So the lateral trunk this replaces was NOT the evidence-based default -- it
 * was what C0 happened to build, with incumbency rather than support. Both
 * readings sat at exactly the same evidential level, which was none.
 *
 * THE QUESTION IS ANSWERABLE AND THIS ENVIRONMENT CANNOT ANSWER IT.
 * Toiviainen 2010's eigenmovements come from a PCA over marker positions, so
 * each one has a DIRECTION by construction -- the plane information is in that
 * paper's own tables and this repo's paraphrase simply did not record it.
 * Frontiers, PMC, doi.org and Wikipedia are all refused by the egress proxy
 * here, the same wall AC2.1 has been behind since C2. Resolving this is an
 * AC2.1-class task: replace this comment with an author/year/TABLE citation.
 *
 * ONE LEAD, FLAGGED AS A LEAD AND NOT AS EVIDENCE: the phrase this repo
 * records is "torso SWAY at the bar", and "sway" in the mocap literature
 * conventionally means lateral. Reading a plane off one English word in a
 * second-hand paraphrase is the same laundering that was refused for de Leva's
 * segment masses. Check the source; do not cite this sentence.
 *
 * **IT SHIPPED `false` FOR ONE COMMIT, AND THE REASON IS WORTH KEEPING.**
 * Flipping it made `feel_flow_puppet_chain`'s inheritance rows VACUOUS: they
 * assert that the head stub, the shoulder bar and both arms carry the trunk's
 * lean, and every one of those readouts was FRONTAL. With the trunk driven
 * sagittally they all read 0 against a trunk of 0 -- eight assertions passing
 * while measuring nothing. A gate that cannot fail is worse than no gate.
 *
 * The fix was `headStubFlexDeg` plus a PLANE-AWARE inheritance check. It also
 * asserts that the other two planes stay at zero -- which was claimed here as
 * "strictly stronger" and then MEASURED FALSE: an injected 0.4-degree leak is
 * caught with that assertion and without it, because the case's explicit
 * three-axis rows already pin all three readouts. It adds coverage only on the
 * inheritTol rows, which otherwise assert one axis. Kept on that basis.
 *
 * WHAT THE DECISION IS ACTUALLY BASED ON, stated so it can be weighed: a
 * dated, single-subject introspective report -- the requester, twice
 * independently, that fore-aft upper-body motion is the primary reaction of a
 * standing listener and that the shipped model showed circular/left-right hip
 * motion with no forward-back trunk motion at all. Weak evidence, but recorded
 * AS n=1 rather than laundered into the model as fact, which is the
 * distinction proposal 44's whole thesis depends on.
 */
constexpr bool   kTrunkSagittal = true;

}   // namespace sfeelflowskel

/**
 * The joint excursions. The five that the pose drives are normalized to
 * [-1,1]; `trunkFlex` and `trunkTwist` are the two axes the shipped model did
 * not have, present and wired but not yet driven (see "what this does not buy").
 */
struct SFeelFlowJoints {
    float bounceY    = 0.0f;   // pelvis vertical, + is a LIFT
    float sway       = 0.0f;   // trunk LATERAL flexion  (about z, frontal)
    float armSwing   = 0.0f;   // arm swing, SAGITTAL    (about x), antiphase
    float headNod    = 0.0f;   // head angle RELATIVE TO THE TRUNK, lateral
    float hipShift   = 0.0f;   // pelvis lateral translation
    float trunkFlex  = 0.0f;   // trunk SAGITTAL flexion (about x)  -- undriven
    float trunkTwist = 0.0f;   // trunk AXIAL rotation   (about y)  -- undriven

    /**
     * WHICH PLANE the `sway` scalar (and with it `headNod`) acts in. Defaults
     * to sfeelflowskel::kTrunkSagittal, which is the project-wide decision.
     *
     * IT IS A FIELD AND NOT JUST THAT CONSTANT, because making the plane a
     * compile-time fork COSTS TWO 3D-COMPOSITION GATES. With `sway` sagittal
     * it becomes COPLANAR with `trunkFlex`, so the pair simply adds (20 + 25 =
     * 45.0000, measured) and the cross-term that proved rotations compose as
     * rotations rather than as numbers -- 21.8802 / 25.0000 / -8.7447 -- has
     * nothing left to measure. Same for the arm: 36.6915 becomes a plain
     * 20 + 35 = 55. Those pins are the difference between a 3D body and two
     * flat drawings stacked, and losing them to a plane decision would be a
     * bad trade.
     *
     * As a field, every one of them survives verbatim at `false` while the
     * shipped default is `true` -- and the plane decision itself becomes
     * testable both ways instead of being a constant nobody can exercise.
     */
    bool  sagittalTrunk = sfeelflowskel::kTrunkSagittal;
};

/** The skeleton in BODY space, plus the angles a gate asserts. */
struct SFeelFlowSkeleton {
    bool valid = false;

    SFeelFlowVec3 pelvis, neck, headCentre, headBase;
    SFeelFlowVec3 shoulderL, shoulderR, armEndL, armEndR;
    SFeelFlowVec3 footL, footR, hipL, hipR;
    double headRadius = 0.0;
    double groundY    = 0.0;

    /**
     * Recovered GEOMETRICALLY from the points above, never echoed back from the
     * inputs that produced them -- an assertion against a number the function
     * was told would be tautological.
     *
     * Each is measured in the plane that segment's own DOF acts in, and the
     * LATERAL ones are what the kinematic-chain gate uses: with `armSwing`,
     * `headNod`, `trunkFlex` and `trunkTwist` all zero, the four below are all
     * EQUAL to `trunkLeanDeg`. That identity is C0's gate and it survives the
     * move to 3D unchanged, which is the point of keeping these scalars.
     */
    double trunkLeanDeg    = 0.0;   // frontal:  pelvis -> neck, from world up
    double headStubLeanDeg = 0.0;   // frontal:  neck -> head base
    double shoulderBarDeg  = 0.0;   // frontal:  shoulderL -> shoulderR
    double armLeanLDeg     = 0.0;   // frontal:  shoulder -> arm end
    double armLeanRDeg     = 0.0;

    /** Sagittal components. `trunkFlexDeg` is 0 until something drives it;
     * the arm swings are the DOF that actually moved plane in C3. */
    double trunkFlexDeg    = 0.0;
    /**
     * SAGITTAL: neck -> head base. The twin of `headStubLeanDeg`, added for
     * proposal 44 C7 because without it the plane switch could not be GATED:
     * every readout the inheritance assertion compares was frontal, so a
     * sagittally-driven trunk left all four at zero and eight assertions would
     * have passed while measuring nothing.
     *
     * NOTE WHAT IS DELIBERATELY ABSENT: there is no sagittal shoulder-BAR
     * twin, and that is geometry rather than an omission. The bar lies ALONG
     * the x axis, which is the axis sagittal flexion rotates about, so a
     * forward lean carries the bar forward without turning it -- its angle is
     * invariant by construction. The sagittal inheritance set is therefore
     * {head stub, both arms}, three rows where the frontal set has four.
     */
    double headStubFlexDeg = 0.0;
    double armSwingLDeg    = 0.0;
    double armSwingRDeg    = 0.0;

    /** Axial rotation, read off the shoulder bar in the TRANSVERSE (x-z)
     * plane. Nothing else reports it: a twist leaves both the frontal and the
     * sagittal angles at zero and only changes which way the bar POINTS, so
     * without this the third axis would be present in the model and invisible
     * to every assertion. It is also the foreshortening a front view cannot
     * show, which is half of why the shipped figure could not express it. */
    double shoulderTwistDeg = 0.0;
};

/** Where the camera looks from. Orthographic on purpose: a verification device
 * should not add perspective foreshortening to the thing being verified, and an
 * orthographic projection keeps every measurement on screen linear in the
 * model. Depth reads from the three-quarter angle instead. */
struct SFeelFlowCamera {
    double azimuthDeg   = 28.0;   // 0 = straight on; + turns the body's left toward us
    double elevationDeg = 10.0;   // + looks down on the figure
};

/** A projected wireframe: polylines in widget coordinates. One polyline per
 * drawn element, so the painter neither knows nor decides what a "limb" is. */
struct SFeelFlowWire {
    enum Part { Legs = 0, Trunk, Arms, Head, Hips, Ground, PartCount };
    Part                 part = Legs;
    std::vector<QPointF> pts;          // >= 2 points
    double               depth = 0.0;  // mean z in body space, for painter order
};

/** The full excursion of each joint, in degrees or as a fraction of the box.
 * Display constants: nothing downstream reads them and no gate pins them. */
namespace sfeelflowskel {


constexpr double kSwayDeg    = 20.0;
constexpr double kNodDeg     = 10.0;
constexpr double kArmDeg     = 35.0;
constexpr double kFlexDeg    = 25.0;   // trunk sagittal, when driven
constexpr double kTwistDeg   = 20.0;   // trunk axial, when driven
constexpr double kBounceFrac = 0.06;
constexpr double kHipFrac    = 0.08;
}

/** The 3D skeleton for `j`, in body space, scaled to `box`. */
SFeelFlowSkeleton sFeelFlowSkeletonFor( const SFeelFlowJoints &j, const QRectF &box );

/** Projects `sk` into `box` through `cam`, back-to-front by depth. */
std::vector<SFeelFlowWire> sFeelFlowProject( const SFeelFlowSkeleton &sk,
                                             const QRectF &box,
                                             const SFeelFlowCamera &cam );

/** The describe() grammar the verb and any diagnostic share -- ONE spelling,
 * 4 decimals, C locale unconditionally (this box's LC_NUMERIC is de_DE and a
 * decimal COMMA would break every `contains=` downstream). */
QString sFeelFlowSkeletonDescribe( const SFeelFlowSkeleton &s );

#endif // SFEELFLOWSKELETON_H
