#include "app/timeline/sfeelflowpuppet.h"

#include "app/model/sfeelflowskeleton.h"

#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QPolygonF>

#include <vector>

#include <algorithm>
#include <cmath>

namespace {

// The joint excursions and the box fractions now live with the ONE function
// that uses them (app/model/sfeelflowskeleton.h). This widget owns no geometry
// at all -- see that header for why, and for the defect it fixes.

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

    // EVERY point comes from the one pure function; this widget computes no
    // geometry, no angles and no trigonometry of its own. A pose component of
    // 0 IS the rest position, so an invalid pose needs no separate "neutral
    // figure" path -- only the dim palette.
    SFeelFlowJoints j;
    j.bounceY  = pose_.bounceY;
    j.sway     = pose_.sway;
    j.armSwing = pose_.armSwing;
    j.headNod  = pose_.headNod;
    j.hipShift = pose_.hipShift;
    const SFeelFlowSkeleton sk = sFeelFlowSkeletonFor( j, box );
    if( !sk.valid ) return;
    const std::vector<SFeelFlowWire> wires = sFeelFlowProject( sk, box, camera_ );

    p.fillRect( rect(), QColor( 26, 28, 32 ) );
    const bool dimmed = !pose_.valid;

    // Which pose energy lights which wire. The ground has none -- it is not a
    // body part and must not brighten when the body does.
    auto energyFor = [&]( SFeelFlowWire::Part part ) -> float {
        switch( part ) {
            case SFeelFlowWire::Legs:  return pose_.energy[SFeelFlowPose::PartBounce];
            case SFeelFlowWire::Trunk: return pose_.energy[SFeelFlowPose::PartSway];
            case SFeelFlowWire::Arms:  return pose_.energy[SFeelFlowPose::PartLimbs];
            case SFeelFlowWire::Head:  return pose_.energy[SFeelFlowPose::PartReference];
            case SFeelFlowWire::Hips:  return pose_.energy[SFeelFlowPose::PartTwobar];
            default:                   return 0.0f;
        }
    };

    // Back to front, as the projection ordered them.
    for( const SFeelFlowWire &w : wires ) {
        if( w.pts.size() < 2 ) continue;
        if( w.part == SFeelFlowWire::Ground ) {
            p.setPen( QPen( QColor( dimmed ? 44 : 58, dimmed ? 44 : 58,
                                    dimmed ? 48 : 66 ), 1.0 ) );
        } else {
            p.setPen( partPen( energyFor( w.part ), dimmed ) );
        }
        p.drawPolyline( w.pts.data(), (int) w.pts.size() );
    }

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
