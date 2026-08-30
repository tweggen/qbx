#ifndef _TWBODYPOSEASPECT_H_
#define _TWBODYPOSEASPECT_H_

#include <cstdint>
#include <vector>

#include "tw/sidecar/twgrooveaspect.h"
#include "tw/body/twbodymeasures.h"

/**
 * Proposal 44 C5 -- the "body.pose" aspect: the BODY's own trajectory, as a
 * plant driven by the ensemble rather than as a display mapping of it.
 *
 * The wire format is twaspects.h's; this header is the params blob and the
 * decoder. Read twaspects.h's "body.pose" entry first -- it carries the design
 * argument, and the argument is the point.
 */

/** The DOF set, in the skeleton's own order (SFeelFlowJoints). NORMATIVE for
 * the payload: a record is nDof*3 floats in exactly this sequence. */
enum class twBodyPoseDof : uint8_t {
    BounceY = 0, Sway, ArmSwing, HeadNod, HipShift, TrunkFlex, TrunkTwist,
    Count
};

/** One DOF at one hop. */
struct twBodyPoseSample {
    float angle        = 0.0f;   // rad, or metres for a translational DOF
    float velocity     = 0.0f;   // rad/s, or m/s
    /** The TOTAL muscle torque: postural tone PLUS the ensemble's own active
     * torque. The sum, never the active half alone -- an effort measure that
     * counted only the active part reports a body straining against a beat as
     * cheaper than one merely standing leant over (tw/body CONTRACT inv. 22). */
    float muscleTorque = 0.0f;   // N*m
};

/** One decoded "body.pose" record: every DOF at one hop, in enum order. */
struct twBodyPoseRecord {
    twBodyPoseSample dof[(int) twBodyPoseDof::Count];
};

/**
 * The params blob for "body.pose".
 *
 * **IT NESTS THE GROOVE BLOB RATHER THAN EXTENDING IT, and that is the whole
 * design.** `twGrooveAnalysisParams::serialize` is not touched by proposal 44
 * at any point. Appending M and H to it would silently re-key "groove.res",
 * "groove.ev" and "groove.dyn" too -- all three share that one params hash --
 * so the first user with a non-default body would trigger a full groove
 * re-analysis for a parameter the ensemble does not consume. Nesting makes the
 * containment structural instead: a GROOVE parameter change re-keys both
 * (correct, the plant is driven by the ensemble), a BODY parameter change
 * re-keys only this aspect (also correct, the ensemble does not know the body
 * exists).
 */
struct twBodyPoseParams {
    twGrooveAnalysisParams groove;
    twBodyMeasures         body;

    /** The groove blob's bytes VERBATIM, then f64 massKg, f64 statureM.
     * Field order normative. */
    void serialize( std::vector<uint8_t> &out ) const;
};

/** Encodes one run's per-hop DOF trajectories into a "body.pose" payload.
 * `records` is hop-major; every record carries every DOF. */
void twBodyPoseEncode( const std::vector<twBodyPoseRecord> &records,
                       std::vector<uint8_t> &out );

/** Decodes a "body.pose" payload. Returns empty on a stride/size mismatch --
 * which is also what a payload written at a different nDof produces, and it
 * cannot arrive: the aspect VERSION is part of the store key, so an older
 * entry MISSES the load() rather than being decoded at the wrong stride. */
std::vector<twBodyPoseRecord> twBodyPoseDecode( const uint8_t *payload,
                                                uint64_t payloadLen );

#endif
