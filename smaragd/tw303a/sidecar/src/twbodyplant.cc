#include "tw/sidecar/twbodyplant.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr int kFlex    = (int) twBodyAxis::Flex;
constexpr int kLateral = (int) twBodyAxis::Lateral;

/** Which of the four joints a DOF lives on, and on which axis. */
struct DofMap {
    twBodyPoseDof dof;
    int           joint;        // index into the joints below
    twBodyAxis    axis;
    const char   *unit;         // "" == driven by no unit at all
};

enum { JLeg = 0, JTrunk = 1, JArm = 2, JNeck = 3, JCount = 4 };

const DofMap kDofs[] = {
    { twBodyPoseDof::BounceY,    JLeg,   twBodyAxis::Flex,    "bounce" },
    { twBodyPoseDof::HipShift,   JLeg,   twBodyAxis::Lateral, "twobar" },
    { twBodyPoseDof::Sway,       JTrunk, twBodyAxis::Lateral, "sway"   },
    { twBodyPoseDof::TrunkFlex,  JTrunk, twBodyAxis::Flex,    ""       },
    { twBodyPoseDof::TrunkTwist, JTrunk, twBodyAxis::Axial,   ""       },
    { twBodyPoseDof::ArmSwing,   JArm,   twBodyAxis::Flex,    "limbs"  },
    // THE NECK IS DRIVEN BY NOTHING. Its motion comes entirely from its
    // parent -- see the header. This empty string is the proposal's whole
    // point expressed as data.
    { twBodyPoseDof::HeadNod,    JNeck,  twBodyAxis::Lateral, ""       },
};
constexpr int kDofCount = (int) ( sizeof( kDofs ) / sizeof( kDofs[0] ) );

} // namespace

double twBodyPoseRangeRad( twBodyPoseDof dof )
{
    // Provisional ranges, and VERIFY like every other constant in proposal 44 --
    // but they are only a NORMALISATION: an angle is stored in radians and this
    // divides it for a display that wants [-1,1]. A wrong value here makes the
    // puppet's excursion wrong, never its physics.
    switch( dof ) {
        case twBodyPoseDof::BounceY:    return 0.35;   // leg flexion in a bounce
        case twBodyPoseDof::Sway:       return 0.45;   // trunk lateral flexion
        case twBodyPoseDof::ArmSwing:   return 1.20;   // a swinging arm goes far
        case twBodyPoseDof::HeadNod:    return 0.80;   // cervical lateral range
        case twBodyPoseDof::HipShift:   return 0.20;   // a standing sway is small
        case twBodyPoseDof::TrunkFlex:  return 0.60;
        case twBodyPoseDof::TrunkTwist: return 0.70;
        default:                        return 1.0;
    }
}

std::vector<twBodyPoseRecord> twBodyPlantRun( const twBodyPlantInput &in,
                                              const twBodyMeasures &measures,
                                              twBodyPlantReport *report,
                                              double posturalGain,
                                              double stiffnessScale,
                                              double dampingRatio )
{
    std::vector<twBodyPoseRecord> out;
    if( report ) *report = twBodyPlantReport{};
    const size_t nHops = in.dyn.size();
    if( nHops == 0 || in.nUnits == 0 || in.dtSec <= 0.0 ) return out;

    // --- the four joints ---------------------------------------------------
    twBodyJoint joints[JCount];
    {
        twBodyJoint &leg = joints[JLeg];
        leg.segment          = measures.compound( twBodySeg::Thigh, 3 );
        leg.freeAxes         = twBodySpherical();
        leg.invertedPendulum = true;    // it carries the body above it
        leg.parentLever      = 0.0;     // the ground is not moving
        twBodyJoint &trunk = joints[JTrunk];
        trunk.segment          = measures.segment( twBodySeg::Trunk );
        trunk.freeAxes         = twBodySpherical();
        trunk.invertedPendulum = true;  // the trunk's mass is above the hip
        trunk.parentLever      = measures.compound( twBodySeg::Thigh, 3 ).length;
        twBodyJoint &arm = joints[JArm];
        arm.segment          = measures.compound( twBodySeg::UpperArm, 3 );
        arm.freeAxes         = twBodySpherical();
        arm.invertedPendulum = false;   // an arm HANGS
        arm.parentLever      = measures.segment( twBodySeg::Trunk ).length;
        twBodyJoint &neck = joints[JNeck];
        neck.segment          = measures.segment( twBodySeg::HeadNeck );
        neck.freeAxes         = twBodySpherical();
        neck.invertedPendulum = true;   // the head sits ABOVE the atlas
        neck.parentLever      = measures.segment( twBodySeg::Trunk ).length;
        for( int j = 0; j < JCount; j++ ) {
            joints[j].stiffnessScale = stiffnessScale;
            joints[j].dampingRatio   = dampingRatio;
            joints[j].posturalGain   = posturalGain;
        }
    }

    // --- each DOF's unit, its smoother, and its DERIVED torque scale --------
    struct DofState {
        int    unit        = -1;
        double torqueScale = 0.0;
        twBodyUrgeSmoother urge;
    };
    DofState st[kDofCount];
    uint32_t driven = 0;
    for( int d = 0; d < kDofCount; d++ ) {
        const DofMap &m = kDofs[d];
        if( m.unit[0] != '\0' ) {
            for( uint32_t u = 0; u < in.nUnits && u < in.unitNames.size(); u++ )
                if( in.unitNames[u] == m.unit ) { st[d].unit = (int) u; break; }
            if( st[d].unit >= 0 ) driven++;
        }
        // FULL URGE AT RESONANCE IS FULL RANGE. Derived, not chosen: the
        // steady-state amplitude at resonance is Q*tau/k, so the torque that
        // reaches the range of motion is rom*k/Q. See the header.
        const twBodyParentMotion still;
        const double k    = twBodyJointStiffness( joints[m.joint], m.axis );
        const double zEff = twBodyJointDampingRatioAt( joints[m.joint], m.axis, still );
        const double Q    = zEff > 0.0 ? 1.0 / ( 2.0 * zEff ) : 1.0;
        if( k > 0.0 && Q > 0.0 )
            st[d].torqueScale = twBodyPoseRangeRad( m.dof ) * k / Q;
    }

    // --- integrate ---------------------------------------------------------
    twBodyJointState js[JCount];
    double prevVel[JCount][(int) twBodyAxis::Count] = {};
    out.resize( nHops );
    double peakUrge = 0.0, peakNod = 0.0, peakSway = 0.0;

    for( size_t hop = 0; hop < nHops; hop++ ) {
        const twGrooveDynRecord &rec = in.dyn[hop];

        // The active torque per DOF, this hop.
        double tau[JCount][(int) twBodyAxis::Count] = {};
        for( int d = 0; d < kDofCount; d++ ) {
            if( st[d].unit < 0 ) continue;
            const size_t u = (size_t) st[d].unit;
            if( u >= rec.units.size() || u >= in.unitK.size() ) continue;
            const double drive = twBodyDrive( rec.units[u].support,
                                              rec.units[u].tension, in.unitK[u] );
            const double urge  = st[d].urge.step( drive, in.dtSec );
            peakUrge = std::max( peakUrge, urge );
            // THE MUSIC AS A TORQUE: amplitude from how much it is asking,
            // SHAPE from the unit's own metrical phase. cosMet, never cosPhi --
            // arg(z) is drive-locked and would swing every joint at the tatum
            // (proposal 44 C1a).
            tau[kDofs[d].joint][(int) kDofs[d].axis] +=
                st[d].torqueScale * urge * (double) rec.units[u].cosMet;
        }

        // The chain, PARENT FIRST. Each child is driven by its parent's angle,
        // velocity and acceleration -- the acceleration by finite difference of
        // the parent's own velocity, which is the only place a derivative is
        // taken and is why the order below matters.
        auto parentOf = [&]( int joint ) {
            twBodyParentMotion p;
            int par = -1;
            if( joint == JTrunk ) par = JLeg;
            else if( joint == JArm || joint == JNeck ) par = JTrunk;
            if( par < 0 ) return p;
            for( int a = 0; a < (int) twBodyAxis::Count; a++ ) {
                p.angle[a]  = js[par].angle[a];
                p.angVel[a] = js[par].vel[a];
                p.angAcc[a] = ( js[par].vel[a] - prevVel[par][a] ) / in.dtSec;
            }
            return p;
        };

        const int order[JCount] = { JLeg, JTrunk, JArm, JNeck };
        for( int oi = 0; oi < JCount; oi++ ) {
            const int j = order[oi];
            const twBodyParentMotion p = parentOf( j );
            for( int a = 0; a < (int) twBodyAxis::Count; a++ )
                prevVel[j][a] = js[j].vel[a];
            twBodyJointStep( joints[j], js[j], p, tau[j], in.dtSec );
        }

        // Report every DOF: angle, velocity, and the TOTAL muscle torque --
        // postural tone PLUS the active half, because an effort measure that
        // counted only the active part would call a body straining against a
        // beat cheaper than one standing leant over (tw/body CONTRACT 22).
        twBodyPoseRecord &r = out[hop];
        for( int d = 0; d < kDofCount; d++ ) {
            const DofMap &m = kDofs[d];
            const int a = (int) m.axis;
            const twBodyParentMotion p = parentOf( m.joint );
            r.dof[(int) m.dof].angle    = (float) js[m.joint].angle[a];
            r.dof[(int) m.dof].velocity = (float) js[m.joint].vel[a];
            r.dof[(int) m.dof].muscleTorque =
                (float) ( tau[m.joint][a]
                          + twBodyPosturalTorque( joints[m.joint], m.axis, p,
                                                  js[m.joint] ) );
        }
        peakNod  = std::max( peakNod,  std::fabs( js[JNeck].angle[kLateral] ) );
        peakSway = std::max( peakSway, std::fabs( js[JTrunk].angle[kLateral] ) );
    }

    if( report ) {
        report->hops          = (uint32_t) nHops;
        report->drivenDof     = driven;
        report->peakUrge      = peakUrge;
        report->peakHeadNod   = peakNod;
        report->peakTrunkSway = peakSway;
        for( int d = 0; d < kDofCount; d++ )
            report->torqueScale[(int) kDofs[d].dof] = st[d].torqueScale;
    }
    (void) kFlex;
    return out;
}
