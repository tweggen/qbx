#include "app/model/sfeelflowskeleton.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr double kPi = 3.14159265358979323846;
inline double rad( double deg ) { return deg * kPi / 180.0; }

// ---- the three anatomical rotations, named by what they DO ----------------
// A rotation is applied to a body-space OFFSET; composing them is what makes a
// child segment inherit its parent's orientation.

/** Flexion/extension: about x (mediolateral). + bends the body FORWARD. */
inline SFeelFlowVec3 rotFlex( SFeelFlowVec3 v, double deg )
{
    const double c = std::cos( rad( deg ) ), s = std::sin( rad( deg ) );
    return { v.x, v.y * c - v.z * s, v.y * s + v.z * c };
}

/** Lateral flexion: about z (anteroposterior). + leans to the viewer's right. */
inline SFeelFlowVec3 rotLateral( SFeelFlowVec3 v, double deg )
{
    const double c = std::cos( rad( deg ) ), s = std::sin( rad( deg ) );
    return { v.x * c + v.y * s, -v.x * s + v.y * c, v.z };
}

/**
 * Axial rotation: about y (longitudinal).
 *
 * + sends +x AWAY from the viewer (x -> z = -x*sin), i.e. it turns the
 * VIEWER-RIGHT side of the body away and brings the viewer-left side forward.
 * This header said "+ turns the body's left toward us", which contradicts the
 * arithmetic under this file's own conventions (+x = the viewer's right, per
 * frontalDeg; +z = toward the viewer, per rotFlex's "+ = forward").
 *
 * CONSEQUENCE, because it surprises people and is pinned in the qxa case: the
 * shoulder-bar TRANSVERSE READOUT comes out with the OPPOSITE SIGN to the
 * commanded twist. shoulderTwistDeg is atan2(zR - zL, xR - xL) over a bar
 * lying along +x, so a +20 degree twist reads -20. That inversion is a
 * property of measuring a heading rather than echoing an input, which is the
 * whole point of recovering the angles geometrically.
 */
inline SFeelFlowVec3 rotAxial( SFeelFlowVec3 v, double deg )
{
    const double c = std::cos( rad( deg ) ), s = std::sin( rad( deg ) );
    return { v.x * c + v.z * s, v.y, -v.x * s + v.z * c };
}

inline SFeelFlowVec3 add( SFeelFlowVec3 a, SFeelFlowVec3 b )
{ return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline SFeelFlowVec3 sub( SFeelFlowVec3 a, SFeelFlowVec3 b )
{ return { a.x - b.x, a.y - b.y, a.z - b.z }; }

/**
 * The TRUNK's orientation, as one transform applied to a trunk-local offset.
 * Order matters and is stated once here: twist about the body's own long axis
 * first, then lateral lean, then forward flexion. Every segment above the
 * pelvis goes through this, which is what makes the chain a chain.
 */
inline SFeelFlowVec3 trunkFrame( SFeelFlowVec3 local, double flexDeg,
                                 double lateralDeg, double twistDeg )
{
    return rotFlex( rotLateral( rotAxial( local, twistDeg ), lateralDeg ), flexDeg );
}

/** Angle of a segment from world-UP, measured in the FRONTAL (x-y) plane.
 * + = leaning to the viewer's right, matching kSwayDeg's sign. */
inline double frontalDeg( SFeelFlowVec3 from, SFeelFlowVec3 to )
{
    const SFeelFlowVec3 d = sub( to, from );
    return std::atan2( d.x, d.y ) * 180.0 / kPi;
}

/** Angle from world-UP in the SAGITTAL (z-y) plane. + = forward. */
inline double sagittalDeg( SFeelFlowVec3 from, SFeelFlowVec3 to )
{
    const SFeelFlowVec3 d = sub( to, from );
    return std::atan2( d.z, d.y ) * 180.0 / kPi;
}

/** A hanging segment reads its rotation mirrored -- a rigid rotation moves an
 * up-vector and a down-vector oppositely, so recovering theta from a limb that
 * points DOWN needs the argument order flipped. */
inline double frontalDegDown( SFeelFlowVec3 from, SFeelFlowVec3 to )
{
    const SFeelFlowVec3 d = sub( to, from );
    return std::atan2( -d.x, -d.y ) * 180.0 / kPi;
}
inline double sagittalDegDown( SFeelFlowVec3 from, SFeelFlowVec3 to )
{
    const SFeelFlowVec3 d = sub( to, from );
    return std::atan2( -d.z, -d.y ) * 180.0 / kPi;
}

} // namespace

SFeelFlowSkeleton sFeelFlowSkeletonFor( const SFeelFlowJoints &j, const QRectF &box )
{
    using namespace sfeelflowskel;
    SFeelFlowSkeleton sk;
    if( box.width() < 8.0 || box.height() < 16.0 ) return sk;
    sk.valid = true;

    const double legLen    = box.height() * 0.30;
    const double torsoLen  = box.height() * 0.30;
    const double headR     = box.height() * 0.070;
    const double armLen    = box.height() * 0.22;
    const double shoulderW = box.width()  * 0.12;
    const double hipW      = box.width()  * 0.09;

    sk.headRadius = headR;
    sk.groundY    = 0.0;                       // body space: the floor is y = 0

    // The pelvis TRANSLATES and does not rotate: the trunk turns ABOUT it, so
    // the hip bar stays level by construction. A pelvis that inherited the
    // trunk's lean would be a second joint nobody asked for.
    sk.pelvis = { (double) j.hipShift * box.width() * kHipFrac,
                  legLen + (double) j.bounceY * box.height() * kBounceFrac,
                  0.0 };

    // The `sway` scalar's PLANE is one declared switch (kTrunkSagittal), and
    // the whole of the plane decision lives at that constant rather than being
    // spread across a mapping. `trunkFlex` keeps its own axis either way: it is
    // an independent DOF, not an alias for this one.
    const double swayDeg    = (double) j.sway       * kSwayDeg;
    const double flexDeg    = (double) j.trunkFlex  * kFlexDeg
                              + ( j.sagittalTrunk ? swayDeg : 0.0 );
    const double lateralDeg = j.sagittalTrunk ? 0.0 : swayDeg;
    const double twistDeg   = (double) j.trunkTwist * kTwistDeg;

    auto fromTrunk = [&]( SFeelFlowVec3 local ) {
        return trunkFrame( local, flexDeg, lateralDeg, twistDeg );
    };

    sk.neck = add( sk.pelvis, fromTrunk( { 0.0, torsoLen, 0.0 } ) );

    // The head's own nod is RELATIVE to the trunk, so it is applied in
    // TRUNK-LOCAL space and then carried out by the trunk's frame -- which is
    // exactly what "the head is subordinate to the torso" means as a transform.
    // IN THE SAME PLANE AS THE TRUNK IT HANGS ON. A head that nodded laterally
    // off a trunk bending fore-aft would be a shrug, not a nod -- and the
    // requester's own account of the motion ("pulling it suddenly back up,
    // letting it fall with torso movement") is unambiguously sagittal.
    const double nodDeg = (double) j.headNod * kNodDeg;
    auto fromNeck = [&]( SFeelFlowVec3 local ) {
        const SFeelFlowVec3 nodded = j.sagittalTrunk ? rotFlex( local, nodDeg )
                                                     : rotLateral( local, nodDeg );
        return add( sk.neck, fromTrunk( nodded ) );
    };
    sk.headCentre = fromNeck( { 0.0, headR * 1.35, 0.0 } );
    sk.headBase   = fromNeck( { 0.0, headR * 0.35, 0.0 } );

    sk.shoulderL = add( sk.neck, fromTrunk( { -shoulderW, -torsoLen * 0.06, 0.0 } ) );
    sk.shoulderR = add( sk.neck, fromTrunk( {  shoulderW, -torsoLen * 0.06, 0.0 } ) );

    // THE ARMS ARE SAGITTAL. This is the DOF that changed plane in C3: it was
    // drawn as lateral abduction -- a jumping-jack -- and an arm swing is
    // fore-aft. Antiphase, because one unit drives both arms and a walking body
    // swings them opposite; in phase they would be unreadable off the figure.
    const double armDeg = (double) j.armSwing * kArmDeg;
    sk.armEndL = add( sk.shoulderL, fromTrunk( rotFlex( { 0.0, -armLen, 0.0 }, +armDeg ) ) );
    sk.armEndR = add( sk.shoulderR, fromTrunk( rotFlex( { 0.0, -armLen, 0.0 }, -armDeg ) ) );

    // The ground is the one thing that does not move: the feet stay planted and
    // the legs express the pelvis's own travel.
    sk.footL = { -hipW, 0.0, 0.0 };
    sk.footR = {  hipW, 0.0, 0.0 };
    sk.hipL  = add( sk.pelvis, { -hipW, 0.0, 0.0 } );
    sk.hipR  = add( sk.pelvis, {  hipW, 0.0, 0.0 } );

    // --- recovered from the POINTS ----------------------------------------
    sk.trunkLeanDeg    = frontalDeg( sk.pelvis, sk.neck );
    sk.trunkFlexDeg    = sagittalDeg( sk.pelvis, sk.neck );
    sk.headStubLeanDeg = frontalDeg( sk.neck, sk.headBase );
    sk.headStubFlexDeg = sagittalDeg( sk.neck, sk.headBase );
    // NEGATED because body space is y-UP while the 2D original measured this in
    // SCREEN space, y-down. Same geometry, opposite sign; the inheritance gate
    // compares this against trunkLeanDeg and would have read -20 against +20.
    sk.shoulderBarDeg  = -std::atan2( sk.shoulderR.y - sk.shoulderL.y,
                                      sk.shoulderR.x - sk.shoulderL.x ) * 180.0 / kPi;
    sk.armLeanLDeg     = frontalDegDown( sk.shoulderL, sk.armEndL );
    sk.armLeanRDeg     = frontalDegDown( sk.shoulderR, sk.armEndR );
    sk.armSwingLDeg    = sagittalDegDown( sk.shoulderL, sk.armEndL );
    sk.armSwingRDeg    = sagittalDegDown( sk.shoulderR, sk.armEndR );
    sk.shoulderTwistDeg = std::atan2( sk.shoulderR.z - sk.shoulderL.z,
                                      sk.shoulderR.x - sk.shoulderL.x ) * 180.0 / kPi;
    return sk;
}

std::vector<SFeelFlowWire> sFeelFlowProject( const SFeelFlowSkeleton &sk,
                                             const QRectF &box,
                                             const SFeelFlowCamera &cam )
{
    std::vector<SFeelFlowWire> out;
    if( !sk.valid ) return out;

    const double ca = std::cos( rad( cam.azimuthDeg ) ), sa = std::sin( rad( cam.azimuthDeg ) );
    const double ce = std::cos( rad( cam.elevationDeg ) ), se = std::sin( rad( cam.elevationDeg ) );

    // ORTHOGRAPHIC. Yaw about the world y, then pitch about the camera x. Screen
    // y grows downward, hence the negation -- the one place that flip happens.
    const double cx = box.center().x();
    const double baseY = box.bottom() - box.height() * 0.04;
    auto proj = [&]( SFeelFlowVec3 v ) {
        const double xr =  v.x * ca + v.z * sa;
        const double zr = -v.x * sa + v.z * ca;
        const double yr =  v.y * ce - zr * se;
        return QPointF( cx + xr, baseY - yr );
    };
    // Camera-space depth, LARGER = NEARER: the yaw-rotated z, then the
    // elevation's own contribution. The elevation term was missing, so with the
    // camera raised the painter order was that of a camera at eye level -- only
    // ever a question of which line crosses which, but wrong for free.
    auto depthOf = [&]( SFeelFlowVec3 v ) {
        return v.y * se + ( -v.x * sa + v.z * ca ) * ce;
    };

    auto seg = [&]( SFeelFlowWire::Part p, SFeelFlowVec3 a, SFeelFlowVec3 b ) {
        SFeelFlowWire w;
        w.part  = p;
        w.pts   = { proj( a ), proj( b ) };
        w.depth = 0.5 * ( depthOf( a ) + depthOf( b ) );
        out.push_back( std::move( w ) );
    };

    // The ground: a plate in the x-z plane, so the floor reads as a floor
    // rather than a line, which is what makes the vertical bounce legible.
    {
        const double g = box.width() * 0.30;
        for( int i = -2; i <= 2; i++ ) {
            const double t = g * i / 2.0;
            seg( SFeelFlowWire::Ground, { -g, 0.0, t }, { g, 0.0, t } );
            seg( SFeelFlowWire::Ground, { t, 0.0, -g }, { t, 0.0, g } );
        }
    }

    seg( SFeelFlowWire::Legs,  sk.pelvis, sk.footL );
    seg( SFeelFlowWire::Legs,  sk.pelvis, sk.footR );
    seg( SFeelFlowWire::Trunk, sk.pelvis, sk.neck );
    seg( SFeelFlowWire::Arms,  sk.shoulderL, sk.armEndL );
    seg( SFeelFlowWire::Arms,  sk.shoulderR, sk.armEndR );
    seg( SFeelFlowWire::Arms,  sk.shoulderL, sk.shoulderR );
    seg( SFeelFlowWire::Head,  sk.neck, sk.headBase );
    seg( SFeelFlowWire::Hips,  sk.hipL, sk.hipR );

    // The head as a WIREFRAME SPHERE: three great circles, one per anatomical
    // plane, so which way the head is facing is readable. They are generated in
    // 3D and projected like everything else -- an ellipse drawn in 2D would not
    // turn with the body, which is the whole thing being verified.
    {
        const int    kN = 24;
        const double r  = sk.headRadius;
        for( int plane = 0; plane < 3; plane++ ) {
            SFeelFlowWire w;
            w.part = SFeelFlowWire::Head;
            double dsum = 0.0;
            for( int i = 0; i <= kN; i++ ) {
                const double a = 2.0 * kPi * i / kN;
                const double u = r * std::cos( a ), v = r * std::sin( a );
                SFeelFlowVec3 p = plane == 0 ? SFeelFlowVec3{ u, v, 0.0 }    // frontal
                                : plane == 1 ? SFeelFlowVec3{ 0.0, u, v }    // sagittal
                                             : SFeelFlowVec3{ u, 0.0, v };   // transverse
                p = add( sk.headCentre, p );
                w.pts.push_back( proj( p ) );
                dsum += depthOf( p );
            }
            w.depth = dsum / (double) ( kN + 1 );
            out.push_back( std::move( w ) );
        }
    }

    // Painter's algorithm: far things first. With a wireframe this only decides
    // which line is drawn over which, but it is what stops a far arm appearing
    // to cross in front of the trunk.
    std::stable_sort( out.begin(), out.end(),
                      []( const SFeelFlowWire &a, const SFeelFlowWire &b ) {
                          return a.depth < b.depth;
                      } );
    return out;
}

QString sFeelFlowSkeletonDescribe( const SFeelFlowSkeleton &s )
{
    auto f4 = []( double v ) { return QString::number( v, 'f', 4 ); };
    return QStringLiteral( "skeleton: valid=%1 trunk=%2 headStub=%3"
                           " shoulderBar=%4 armL=%5 armR=%6"
                           " trunkFlex=%7 headStubFlex=%8 armSwingL=%9"
                           " armSwingR=%10 twist=%11" )
        .arg( s.valid ? 1 : 0 )
        .arg( f4( s.trunkLeanDeg ), f4( s.headStubLeanDeg ),
              f4( s.shoulderBarDeg ), f4( s.armLeanLDeg ), f4( s.armLeanRDeg ),
              f4( s.trunkFlexDeg ), f4( s.headStubFlexDeg ),
              f4( s.armSwingLDeg ), f4( s.armSwingRDeg ),
              f4( s.shoulderTwistDeg ) );
}
