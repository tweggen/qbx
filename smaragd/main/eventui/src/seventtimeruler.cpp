#include "app/eventui/seventtimeruler.h"

#include <algorithm>

#include <QPainter>

#include "app/eventui/seventtimeaxis.h"
#include "app/model/sproject.h"
#include "app/objects/midi/smidieventactions.h"
#include "app/shell/sapplication.h"

#include "tw/events/twtempomap.h"

SEventTimeRuler::SEventTimeRuler( QWidget *parent )
    : QWidget( parent )
{
    setFixedHeight( 18 );
}

void SEventTimeRuler::setTimeAxis( SEventTimeAxis *axis )
{
    if( axis_ == axis ) return;
    if( axis_ ) QObject::disconnect( axis_, nullptr, this, nullptr );
    axis_ = axis;
    if( axis_ )
        QObject::connect( axis_, &SEventTimeAxis::axisChanged,
                          this, QOverload<>::of( &QWidget::update ) );
    update();
}

void SEventTimeRuler::setLeftInset( int px )
{
    if( leftInset_ == px ) return;
    leftInset_ = px;
    update();
}

void SEventTimeRuler::setGrid( const QString &grid )
{
    if( grid_ == grid ) return;
    grid_ = grid;
    update();
}

QSize SEventTimeRuler::sizeHint() const
{
    return QSize( 200, 18 );
}

void SEventTimeRuler::paintEvent( QPaintEvent * )
{
    QPainter p( this );
    p.fillRect( rect(), QColor( 55, 57, 62 ) );
    p.setPen( QColor( 90, 92, 98 ) );
    p.drawLine( 0, height() - 1, width(), height() - 1 );

    SProject *project = SApplication::app().getCurrentProject();
    if( !axis_ || !project ) return;

    // THE tempo authority. Everything below is exact rational arithmetic
    // through it; nothing here multiplies a BPM by anything.
    const twTempoMap &map = project->tempoMap();
    const int srate = project->getSRate();
    const int ppq = map.ppq();
    const qint64 beatTicks = ppq;
    const qint64 barTicks = (qint64) ppq * map.numerator() * 4 / map.denominator();
    if( barTicks <= 0 ) return;

    const qint64 subTicks = SQuantizeNotesAction::gridTicks( grid_, ppq );

    // Which ticks are on screen: the axis is in frames, so one conversion each
    // way and then a walk in ticks (the display domain).
    const offset_t leftFrame = axis_->frameOfX( -leftInset_ );
    const offset_t rightFrame = axis_->frameOfX( width() - leftInset_ );
    const qint64 firstTick =
        (qint64) map.framesToTicks( leftFrame, srate ).ticks().floorToInt();
    const qint64 lastTick =
        (qint64) map.framesToTicks( rightFrame, srate ).ticks().floorToInt();
    if( lastTick <= firstTick ) return;

    // Draw the finest division that is still readable, never more.
    const auto xOfTick = [&]( qint64 t ) {
        const offset_t f = (offset_t) map.ticksToFrames(
            TickPos( (int64_t) t ), srate ).floorToInt();
        return axis_->xOfFrame( f ) + leftInset_;
    };
    const int barPx = xOfTick( barTicks ) - xOfTick( 0 );

    qint64 step = barTicks;
    if( subTicks > 0 && barPx / std::max<qint64>( 1, barTicks / subTicks ) >= 6 )
        step = subTicks;
    else if( barPx / std::max<qint64>( 1, barTicks / beatTicks ) >= 6 )
        step = beatTicks;

    qint64 t = ( firstTick / step ) * step;
    if( t > firstTick ) t -= step;
    for( int guard = 0; guard < 8192 && t <= lastTick + step; ++guard, t += step ) {
        if( t < 0 ) continue;
        const int x = xOfTick( t );
        if( x < 0 || x > width() ) continue;
        const bool onBar = ( t % barTicks ) == 0;
        const bool onBeat = ( t % beatTicks ) == 0;
        p.setPen( onBar ? QColor( 200, 200, 205 )
                        : onBeat ? QColor( 140, 142, 148 )
                                 : QColor( 96, 98, 104 ) );
        p.drawLine( x, onBar ? 2 : onBeat ? 7 : 11, x, height() - 2 );
        if( onBar && barPx >= 28 ) {
            const qint64 bar = t / barTicks + 1;
            p.setPen( QColor( 210, 210, 215 ) );
            p.drawText( x + 2, height() - 5, QString::number( bar ) );
        }
    }
}
