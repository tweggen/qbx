#include "app/model/sfeelflowskeleton.h"

#include <cmath>

namespace {

constexpr double kPi = 3.14159265358979323846;

/** `origin` plus the local offset (dx,dy) rotated by `deg`. This is the SAME
 * transform the widget's old `rotateAbout` applied -- the fix is not a new
 * rotation, it is passing a TORSO-LOCAL offset and the composed angle. */
inline QPointF place( const QPointF &origin, double dx, double dy, double deg )
{
    const double r = deg * kPi / 180.0;
    const double c = std::cos( r ), s = std::sin( r );
    return QPointF( origin.x() + dx * c - dy * s,
                    origin.y() + dx * s + dy * c );
}

/** Angle of an UPWARD segment from world-up, + = clockwise on screen. */
inline double leanFromUp( const QPointF &from, const QPointF &to )
{
    return std::atan2( to.x() - from.x(), from.y() - to.y() ) * 180.0 / kPi;
}

/** Angle of a DOWNWARD segment from world-down, same sign convention: a rigid
 * rotation by theta moves an up-vector and a down-vector oppositely on screen,
 * so recovering theta from a hanging limb needs the mirrored argument order. */
inline double leanFromDown( const QPointF &from, const QPointF &to )
{
    return std::atan2( from.x() - to.x(), to.y() - from.y() ) * 180.0 / kPi;
}

} // namespace

SFeelFlowSkeleton sFeelFlowSkeletonFor( const SFeelFlowJoints &j, const QRectF &box )
{
    using namespace sfeelflowskel;
    SFeelFlowSkeleton sk;
    if( box.width() < 8.0 || box.height() < 16.0 ) return sk;
    sk.valid = true;

    const double cx        = box.center().x();
    const double legLen    = box.height() * 0.30;
    const double torsoLen  = box.height() * 0.30;
    const double headR     = box.height() * 0.070;
    const double armLen    = box.height() * 0.22;
    const double shoulderW = box.width()  * 0.12;
    const double hipW      = box.width()  * 0.09;

    sk.headRadius = headR;
    sk.groundY    = box.bottom() - box.height() * 0.04;

    // The pelvis translates; it does NOT rotate. The trunk turns ABOUT it, so
    // the hip bar stays level by construction -- a pelvis that inherited the
    // trunk's lean would be a second joint nobody asked for.
    const double pelvisX = cx + (double) j.hipShift * box.width() * kHipFrac;
    const double pelvisY = sk.groundY - legLen
                           - (double) j.bounceY * box.height() * kBounceFrac;
    sk.pelvis = QPointF( pelvisX, pelvisY );

    // --- the one angle every segment above the pelvis composes on top of ---
    const double trunkDeg = (double) j.sway * kSwayDeg;

    sk.neck = place( sk.pelvis, 0.0, -torsoLen, trunkDeg );

    // TORSO-LOCAL, all of it. The head's own nod is RELATIVE to the trunk, so
    // its world angle is the sum; the shoulders carry no angle of their own and
    // are simply the trunk's; each arm is the trunk plus its own swing, in
    // antiphase.
    const double headDeg = trunkDeg + (double) j.headNod * kNodDeg;
    sk.headCentre = place( sk.neck, 0.0, -headR * 1.35, headDeg );
    sk.headBase   = place( sk.neck, 0.0, -headR * 0.35, headDeg );

    sk.shoulderL = place( sk.neck, -shoulderW, torsoLen * 0.06, trunkDeg );
    sk.shoulderR = place( sk.neck,  shoulderW, torsoLen * 0.06, trunkDeg );

    const double armDeg = (double) j.armSwing * kArmDeg;
    sk.armEndL = place( sk.shoulderL, 0.0, armLen, trunkDeg + armDeg );
    sk.armEndR = place( sk.shoulderR, 0.0, armLen, trunkDeg - armDeg );

    // The ground is the one thing that does not move: the feet stay planted and
    // the legs express the pelvis's own travel.
    sk.footL = QPointF( cx - hipW, sk.groundY );
    sk.footR = QPointF( cx + hipW, sk.groundY );
    sk.hipL  = QPointF( sk.pelvis.x() - hipW, sk.pelvis.y() );
    sk.hipR  = QPointF( sk.pelvis.x() + hipW, sk.pelvis.y() );

    // --- recovered from the POINTS, never echoed from the inputs -----------
    sk.trunkLeanDeg    = leanFromUp( sk.pelvis, sk.neck );
    sk.headStubLeanDeg = leanFromUp( sk.neck, sk.headBase );
    sk.shoulderBarDeg  = std::atan2( sk.shoulderR.y() - sk.shoulderL.y(),
                                     sk.shoulderR.x() - sk.shoulderL.x() )
                         * 180.0 / kPi;
    sk.armLeanLDeg     = leanFromDown( sk.shoulderL, sk.armEndL );
    sk.armLeanRDeg     = leanFromDown( sk.shoulderR, sk.armEndR );
    return sk;
}

QString sFeelFlowSkeletonDescribe( const SFeelFlowSkeleton &s )
{
    auto f4 = []( double v ) { return QString::number( v, 'f', 4 ); };
    return QStringLiteral( "skeleton: valid=%1 trunk=%2 headStub=%3"
                           " shoulderBar=%4 armL=%5 armR=%6" )
        .arg( s.valid ? 1 : 0 )
        .arg( f4( s.trunkLeanDeg ), f4( s.headStubLeanDeg ),
              f4( s.shoulderBarDeg ), f4( s.armLeanLDeg ), f4( s.armLeanRDeg ) );
}
