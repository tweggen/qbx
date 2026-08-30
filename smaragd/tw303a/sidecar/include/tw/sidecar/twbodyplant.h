#ifndef _TWBODYPLANT_H_
#define _TWBODYPLANT_H_

#include <string>
#include <vector>

#include "tw/sidecar/twbodyposeaspect.h"
#include "tw/sidecar/twgrooveaspect.h"
#include "tw/body/twbodyjoint.h"
#include "tw/body/twbodyobjective.h"

/**
 * Proposal 44 C5 -- THE PLANT PASS: the ensemble's output driving a body, per
 * hop, producing the "body.pose" payload.
 *
 * Pure and Qt-free, deliberately: this is where every piece proposal 44 built
 * separately finally meets, and it should be assertable without an analysis, a
 * store or a widget. `twBodyPlantRun` takes decoded groove data and a body, and
 * returns pose records.
 *
 * ======================= THE CHAIN, IN ONE PLACE =========================
 *
 *   groove.dyn  --hypot/k-->  the MUSIC  --one-pole-->  urgeNorm
 *               --x cosMet-->  an ACTIVE TORQUE
 *               --twBodyJointStep-->  an ANGLE, a VELOCITY, a TOTAL TORQUE
 *
 * Every arrow is a decision recorded elsewhere and gated on its own:
 * `twBodyDrive` and `twBodyUrgeSmoother` (tw/body CONTRACT 30-33), the torque
 * reading of proposal 44 section 8 question 3, `twBodyJointStep`'s equation
 * (CONTRACT 1-8) and the postural split (13-15).
 *
 * ==================== FOUR JOINTS, SEVEN DEGREES OF FREEDOM ==============
 *
 * The per-axis work is what makes this a small table rather than seven
 * independent oscillators:
 *
 *   LEG   compound(Thigh..Foot) at the hip   Flex <- "bounce"  -> BounceY
 *                                            Lateral <- "twobar" -> HipShift
 *   TRUNK Trunk at the pelvis                Lateral <- "sway" -> Sway
 *                                            Flex, Axial undriven -> TrunkFlex,
 *                                                                    TrunkTwist
 *   ARM   compound(UpperArm..Hand) at the    Flex <- "limbs"   -> ArmSwing
 *         shoulder, levered off the trunk
 *   NECK  HeadNeck at the atlas, levered     Lateral <- NOTHING -> HeadNod
 *         off the trunk
 *
 * **THE NECK IS DRIVEN BY NO UNIT AT ALL, and that is the point of the whole
 * proposal.** The requester's report (2026-08-27) was that the head moved with
 * the tatum gauge rather than with the torso; C1 took it off that gauge and
 * hung it on the trunk through a first-order lag; C4a showed a lag CANNOT
 * produce the described nod, because a lag has no momentum. Here the head is a
 * real segment on a real joint whose PARENT IS THE TRUNK: move the trunk and
 * stop, and the head keeps going by its own inertia. Nothing is mapped to it.
 *
 * **WHICH SEGMENT STANDS FOR WHICH DOF IS PROVISIONAL and is stated as such.**
 * A bounce is really knee flexion carrying the whole upper body, and a hip
 * shift is really a standing sway about the ankles; both are modelled here as
 * axes of the leg's own compound pendulum. What is NOT provisional is the
 * physics on each axis -- a real segment's inertia, the right gravity sign, an
 * absolute damping, postural tone, and a parent that actually drives its child.
 * An anatomically exact chain is later work and needs the child-to-parent
 * reaction tw/body's CONTRACT already names as missing.
 *
 * ================= THE TORQUE SCALE IS DERIVED, NOT CHOSEN ===============
 *
 * The one number that could have been a free parameter is not one. Per DOF,
 *
 *     torqueScale = rom * k / Q,     Q = 1 / (2 * zeta_eff)
 *
 * i.e. **full urge, at the joint's own resonance, is exactly full range of
 * motion.** That ties the plant's output to `twBodyObjectiveParams::maxUrge`'s
 * own anchor ("full range, three times a second") by construction rather than
 * by tuning, and it is why nothing here needs a scale constant.
 */

/** Everything the plant reads, decoded. Nothing here is owned. */
struct twBodyPlantInput {
    uint32_t nUnits = 0;
    /** Ensemble order, matching `dyn`'s per-unit slices and `unitPower`. */
    std::vector<std::string> unitNames;
    /** Each unit's coupling constant k -- DIVIDED OUT to get the music (see
     * twGrooveCounterTension::k). A zero or missing entry makes that unit
     * contribute nothing rather than dividing by zero. */
    std::vector<double> unitK;
    /** Hop-major, size nHops*nUnits: the normalised resonance power. Used for
     * nothing but a presence check -- the plant is driven by the DRIVE, never
     * by the response. */
    std::vector<float> unitPower;
    /** One record per hop, ensemble order. THE input. */
    std::vector<twGrooveDynRecord> dyn;
    /** Seconds per hop. The aspect grid is rate/100, so 0.01. */
    double dtSec = 0.01;
};

/** How the plant was configured and what it did -- reported, never guessed at
 * by a reader of the payload. */
struct twBodyPlantReport {
    uint32_t hops          = 0;
    uint32_t drivenDof     = 0;   // how many DOF a unit was actually found for
    double   peakUrge      = 0.0; // the loudest urgeNorm any unit reached
    double   peakHeadNod   = 0.0; // rad, |max| -- the requester's own quantity
    double   peakTrunkSway = 0.0; // rad, |max|
    /** The DERIVED torque scale per DOF, N*m per unit urge: rom*k/Q. Reported
     * so a gate can check the derivation itself rather than inferring it from
     * an excursion -- inferring it does not work, and that was measured: a
     * flat constant of 60 N*m produced excursions comfortably inside every
     * plausible bound on the peak angle. A closed form has to be asserted as
     * a closed form. */
    double   torqueScale[(int) twBodyPoseDof::Count] = {};
};

/**
 * Runs the plant over one analysis.
 *
 * `posturalGain` is 1.0 by default: a dancing body holds itself up, and an
 * untoned inverted joint would droop under the trunk's own lean instead of
 * ringing about it (tw/body CONTRACT 13-14). `stiffnessScale` and
 * `dampingRatio` are the joint defaults.
 */
std::vector<twBodyPoseRecord> twBodyPlantRun( const twBodyPlantInput &in,
                                              const twBodyMeasures &measures,
                                              twBodyPlantReport *report = nullptr,
                                              double posturalGain  = 1.0,
                                              double stiffnessScale = 2.0,
                                              double dampingRatio   = 0.30 );

/** The range of motion a DOF is normalised against, radians. ONE spelling,
 * shared by the plant that produces an angle and the consumer that draws it --
 * two tables would be two chances for the puppet to show a different excursion
 * from the one the physics produced (proposal 41 M7's tagChipRect lesson). */
double twBodyPoseRangeRad( twBodyPoseDof dof );

#endif
