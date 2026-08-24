#include "app/timeline/sfeelflowpuppet.h"

#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QPolygonF>

#include <algorithm>
#include <cmath>

namespace {

// The joint excursions, in DEGREES at full deflection (|component| == 1).
// Deliberately modest: the pose components are normalized amplitudes, and a
// figure whose torso swings 90 degrees reads as a cartoon rather than as a
// readout. These are display constants only -- nothing downstream reads them
// and no gate pins them (aesthetics are explicitly NOT gated, M3e AC 7).
constexpr double kSwayDeg = 20.0;
constexpr double kNodDeg  = 10.0;
constexpr double kArmDeg  = 35.0;

// Fractions of the drawing box.
constexpr double kBounceFrac = 0.06;   // pelvis vertical travel
constexpr double kHipFrac    = 0.08;   // pelvis horizontal travel

QPointF rotateAbout( const QPointF &pivot, const QPointF &p, double deg )
{
    const double r = deg * M_PI / 180.0;
    const double c = std::cos( r ), s = std::sin( r );
    const double dx = p.x() - pivot.x(), dy = p.y() - pivot.y();
    return QPointF( pivot.x() + dx * c - dy * s,
                    pivot.y() + dx * s + dy * c );
}

// One part's pen: the SAME accent hue for every limb, with BRIGHTNESS and
// WIDTH scaled by that part's energy, so a part that is not participating
// recedes rather than disappearing (a missing limb reads as a bug; a dim one
// reads as "quiet", which is what it is).
QPen partPen( float energy, bool dimmed )
{
    const double e = std::max( 0.0f, std::min( 1.0f, energy ) );
    const int lo = dimmed ? 60 : 90;
    const int hi = dimmed ? 110 : 235;
    const int v  = lo + (int) ( ( hi - lo ) * e );
    QPen pen( QColor( v, v, (int) std::min( 255.0, v * 1.05 + 12.0 ) ) );
    pen.setWidthF( 1.6 + 2.2 * e );
    pen.setCapStyle( Qt::RoundCap );
    return pen;
}

} // namespace

SFeelFlowPuppetWidget::SFeelFlowPuppetWidget( QWidget *parent )
    : QWidget( parent )
{
    setMinimumSize( minimumSizeHint() );
}

void SFeelFlowPuppetWidget::setPose( const SFeelFlowPose &pose )
{
    auto moved = []( float a, float b ) {
        return std::fabs( a - b ) > SFeelFlowPuppetWidget::kEpsilon;
    };
    bool changed = pose.valid != pose_.valid
                   || moved( pose.bounceY,  pose_.bounceY )
                   || moved( pose.sway,     pose_.sway )
                   || moved( pose.armSwing, pose_.armSwing )
                   || moved( pose.headNod,  pose_.headNod )
                   || moved( pose.hipShift, pose_.hipShift );
    for( int p = 0; p < SFeelFlowPose::PartCount && !changed; p++ )
        changed = moved( pose.energy[p], pose_.energy[p] );

    pose_ = pose;
    if( changed ) update();
}

void SFeelFlowPuppetWidget::paintEvent( QPaintEvent * )
{
    QPainter p( this );
    p.setRenderHint( QPainter::Antialiasing, true );

    const QRectF box = rect().adjusted( 6, 6, -6, -6 );
    if( box.width() < 8.0 || box.height() < 16.0 ) return;

    p.fillRect( rect(), QColor( 26, 28, 32 ) );

    const bool dimmed = !pose_.valid;

    // --- the ground ----------------------------------------------------
    const double groundY = box.bottom() - box.height() * 0.04;
    p.setPen( QPen( QColor( dimmed ? 52 : 78, dimmed ? 52 : 78,
                            dimmed ? 56 : 86 ), 1.0 ) );
    p.drawLine( QPointF( box.left(), groundY ), QPointF( box.right(), groundY ) );

    // --- the skeleton's rest geometry ----------------------------------
    // Everything is derived from the box, so the figure scales with the dock.
    const double cx        = box.center().x();
    const double legLen    = box.height() * 0.30;
    const double torsoLen  = box.height() * 0.30;
    const double headR     = box.height() * 0.070;
    const double armLen    = box.height() * 0.22;
    const double shoulderW = box.width()  * 0.12;
    const double hipW      = box.width()  * 0.09;

    // A pose component of 0 IS the rest position -- an invalid pose therefore
    // needs no separate "neutral figure" code path, only the dim palette.
    const double pelvisX = cx + pose_.hipShift * box.width()  * kHipFrac;
    // Screen y grows downward and a POSITIVE bounceY is a lift.
    const double pelvisY = groundY - legLen
                           - pose_.bounceY * box.height() * kBounceFrac;
    const QPointF pelvis( pelvisX, pelvisY );

    // Torso: leans about the pelvis.
    QPointF neck = rotateAbout( pelvis, QPointF( pelvisX, pelvisY - torsoLen ),
                                pose_.sway * kSwayDeg );
    // Head: nods about the neck.
    const QPointF headC =
        rotateAbout( neck, QPointF( neck.x(), neck.y() - headR * 1.35 ),
                     pose_.headNod * kNodDeg );

    // --- legs ----------------------------------------------------------
    // Feet stay planted (the ground is the one thing that does not move);
    // the pelvis's own travel is what a leg expresses.
    p.setPen( partPen( pose_.energy[SFeelFlowPose::PartBounce], dimmed ) );
    p.drawLine( pelvis, QPointF( cx - hipW, groundY ) );
    p.drawLine( pelvis, QPointF( cx + hipW, groundY ) );

    // --- torso ---------------------------------------------------------
    p.setPen( partPen( pose_.energy[SFeelFlowPose::PartSway], dimmed ) );
    p.drawLine( pelvis, neck );

    // --- arms, in ANTIPHASE --------------------------------------------
    // The design's own wording: one unit ("limbs") drives BOTH arms, and a
    // walking/dancing body swings them opposite. Drawing them in phase would
    // be a jumping-jack, and would also make the one component impossible to
    // read off the figure (both arms at the same angle is what a rest pose
    // looks like at any amplitude).
    p.setPen( partPen( pose_.energy[SFeelFlowPose::PartLimbs], dimmed ) );
    const QPointF shoulderL( neck.x() - shoulderW, neck.y() + torsoLen * 0.06 );
    const QPointF shoulderR( neck.x() + shoulderW, neck.y() + torsoLen * 0.06 );
    const double armDeg = pose_.armSwing * kArmDeg;
    p.drawLine( shoulderL,
                rotateAbout( shoulderL,
                             QPointF( shoulderL.x(), shoulderL.y() + armLen ),
                             +armDeg ) );
    p.drawLine( shoulderR,
                rotateAbout( shoulderR,
                             QPointF( shoulderR.x(), shoulderR.y() + armLen ),
                             -armDeg ) );
    p.drawLine( shoulderL, shoulderR );

    // --- head ----------------------------------------------------------
    p.setPen( partPen( pose_.energy[SFeelFlowPose::PartReference], dimmed ) );
    p.setBrush( Qt::NoBrush );
    p.drawEllipse( headC, headR, headR );
    p.drawLine( neck, QPointF( headC.x(), headC.y() + headR ) );

    // --- the hip marker (the twobar unit's own part) --------------------
    p.setPen( partPen( pose_.energy[SFeelFlowPose::PartTwobar], dimmed ) );
    p.drawLine( QPointF( pelvis.x() - hipW, pelvis.y() ),
                QPointF( pelvis.x() + hipW, pelvis.y() ) );

    // --- the one-line note, only when there is nothing to show ----------
    if( !pose_.valid ) {
        p.setPen( QColor( 130, 130, 138 ) );
        QFont f = p.font();
        f.setPixelSize( std::max( 8, (int) ( box.height() * 0.045 ) ) );
        p.setFont( f );
        p.drawText( QRectF( box.left(), box.top(), box.width(),
                            box.height() * 0.12 ),
                    Qt::AlignHCenter | Qt::AlignVCenter,
                    tr( "no fresh analysis" ) );
    }
}
