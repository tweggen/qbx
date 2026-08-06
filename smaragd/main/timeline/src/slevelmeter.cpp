#include "app/timeline/slevelmeter.h"

#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>

// A plain QWidget subclass paints nothing from a style sheet (Qt only honours one
// for widgets that draw PE_Widget themselves) while still suppressing the palette
// fill, which would leave whatever was last in the backing store. So paint it
// directly — the STrackHeaderResizer approach, both correct and cheaper.
static const QColor kBackFill  ( 0x1c, 0x1c, 0x1c );
static const QColor kFrameLine ( 0x0f, 0x0f, 0x0f );
static const QColor kZoneGreen ( 0x3c, 0xb0, 0x50 );
static const QColor kZoneAmber ( 0xd8, 0xa8, 0x28 );
static const QColor kZoneRed   ( 0xd0, 0x40, 0x40 );
static const QColor kRmsTint   ( 0xff, 0xff, 0xff, 0x50 );  // over the peak zones
static const QColor kHoldTick  ( 0xf0, 0xf0, 0xf0 );
static const QColor kClipCap   ( 0xff, 0x30, 0x30 );

// Zone boundaries, dBFS. Below -9 green, -9..-1 amber, above -1 red.
static constexpr float kAmberFromDb = -9.0f;
static constexpr float kRedFromDb   = -1.0f;

SLevelMeter::SLevelMeter( QWidget *parent )
    : QWidget( parent )
{
    setToolTip( QStringLiteral( "Level (peak + RMS) — click to clear the clip indicator" ) );
    setOrientation( Qt::Vertical );
}

void SLevelMeter::setOrientation( Qt::Orientation o )
{
    orientation_ = o;
    // Set BOTH axes every time: setFixedWidth pins min == max, so flipping
    // orientation without clearing the other axis leaves the bar stuck at its
    // previous thickness.
    if( o == Qt::Vertical ) {
        setMinimumWidth( BAR_THICKNESS );
        setMaximumWidth( BAR_THICKNESS );
        setMinimumHeight( 24 );
        setMaximumHeight( QWIDGETSIZE_MAX );
    } else {
        setMinimumHeight( BAR_THICKNESS );
        setMaximumHeight( BAR_THICKNESS );
        setMinimumWidth( 24 );
        setMaximumWidth( QWIDGETSIZE_MAX );
    }
    // Geometry changed, so the cached pixel state describes a different bar.
    lastPaintedPeakPx_ = lastPaintedRmsPx_ = lastPaintedHoldPx_ = -1;
    updateGeometry();
    update();
}

int SLevelMeter::barLength() const
{
    // One pixel of frame at each end.
    const int raw = ( orientation_ == Qt::Vertical ? height() : width() ) - 2;
    return raw > 0 ? raw : 0;
}

int SLevelMeter::dbToPx( float db ) const
{
    const twMeterBallisticsConfig &cfg = ballistics_.config();
    const int len = barLength();
    if( len <= 0 ) return 0;

    const float span = cfg.ceilDb - cfg.floorDb;
    if( !( span > 0.0f ) ) return 0;

    float t = ( db - cfg.floorDb ) / span;      // dB-linear, so the scale reads
    if( t <= 0.0f ) return 0;                   // evenly in dB
    if( t >= 1.0f ) return len;
    return (int) ( t * (float) len + 0.5f );
}

void SLevelMeter::pushLevel( const twLevelSample &s, qint64 nowMs )
{
    ballistics_.push( s, (double) nowMs / 1000.0 );
    refresh_();
}

void SLevelMeter::pushIdle( qint64 nowMs )
{
    ballistics_.idle( (double) nowMs / 1000.0 );
    refresh_();
}

void SLevelMeter::resetMeter()
{
    ballistics_.reset();
    refresh_();
}

void SLevelMeter::refresh_()
{
    const int  peakPx = dbToPx( ballistics_.peakDb() );
    const int  rmsPx  = dbToPx( ballistics_.rmsDb() );
    const int  holdPx = dbToPx( ballistics_.holdDb() );
    const bool clip   = ballistics_.clipped();

    if( peakPx == lastPaintedPeakPx_ && rmsPx == lastPaintedRmsPx_ &&
        holdPx == lastPaintedHoldPx_ && clip == lastPaintedClip_ ) {
        return;                       // nothing would change on screen
    }

    // A clip cap appearing or clearing changes the far end of the bar; anything
    // else moves within the band between the old and new positions.
    if( clip != lastPaintedClip_ || lastPaintedPeakPx_ < 0 ) {
        update();
        return;
    }

    int lo = qMin( qMin( peakPx, lastPaintedPeakPx_ ),
                   qMin( qMin( rmsPx, lastPaintedRmsPx_ ),
                         qMin( holdPx, lastPaintedHoldPx_ ) ) );
    int hi = qMax( qMax( peakPx, lastPaintedPeakPx_ ),
                   qMax( qMax( rmsPx, lastPaintedRmsPx_ ),
                         qMax( holdPx, lastPaintedHoldPx_ ) ) );
    lo -= 2; hi += 2;                  // the hold tick is 2 px tall

    if( orientation_ == Qt::Vertical ) {
        // The bar grows upward from the bottom, so a larger px is a smaller y.
        const int yTop = height() - 1 - hi;
        update( 0, yTop, width(), hi - lo + 1 );
    } else {
        update( 1 + lo, 0, hi - lo + 1, height() );
    }
}

void SLevelMeter::paintEvent( QPaintEvent * )
{
    QPainter p( this );
    p.fillRect( rect(), kBackFill );
    p.setPen( kFrameLine );
    p.drawRect( 0, 0, width() - 1, height() - 1 );

    const int len = barLength();
    if( len <= 0 ) return;

    const int peakPx = dbToPx( ballistics_.peakDb() );
    const int rmsPx  = dbToPx( ballistics_.rmsDb() );
    const int holdPx = dbToPx( ballistics_.holdDb() );
    const bool clip  = ballistics_.clipped();

    const int amberAt = dbToPx( kAmberFromDb );
    const int redAt   = dbToPx( kRedFromDb );

    // Paint the lit part in up to three zone-coloured slabs rather than
    // per-pixel: the colour depends only on where a pixel sits on the scale.
    const struct { int from, to; const QColor &c; } zones[3] = {
        { 0,       qMin( peakPx, amberAt ), kZoneGreen },
        { amberAt, qMin( peakPx, redAt ),   kZoneAmber },
        { redAt,   peakPx,                  kZoneRed   },
    };

    auto slab = [&]( int from, int to, const QColor &c, int inset ) {
        if( to <= from ) return;
        if( orientation_ == Qt::Vertical ) {
            const int yBottom = height() - 1 - from;
            p.fillRect( 1 + inset, yBottom - ( to - from ) + 1,
                        width() - 2 - 2 * inset, to - from, c );
        } else {
            p.fillRect( 1 + from, 1 + inset, to - from,
                        height() - 2 - 2 * inset, c );
        }
    };

    for( const auto &z : zones ) slab( z.from, z.to, z.c, 0 );

    // The RMS bar sits INSIDE the peak bar: same zone colours showing through,
    // brightened by a translucent overlay, so one widget carries both readings.
    const int inset = ( ( orientation_ == Qt::Vertical ? width() : height() ) - 2 ) / 4;
    if( rmsPx > 0 && inset > 0 ) slab( 0, rmsPx, kRmsTint, inset );

    // Held peak tick.
    if( holdPx > 0 ) slab( qMax( 0, holdPx - 2 ), holdPx, kHoldTick, 0 );

    // Clip cap at the far end — latched until clicked, never time-based.
    if( clip ) slab( qMax( 0, len - 2 ), len, kClipCap, 0 );

    lastPaintedPeakPx_ = peakPx;
    lastPaintedRmsPx_  = rmsPx;
    lastPaintedHoldPx_ = holdPx;
    lastPaintedClip_   = clip;
}

void SLevelMeter::mousePressEvent( QMouseEvent *e )
{
    if( e->button() == Qt::LeftButton && ballistics_.clipped() ) {
        ballistics_.clearClip();
        refresh_();
        e->accept();
        return;
    }
    QWidget::mousePressEvent( e );
}

void SLevelMeter::resizeEvent( QResizeEvent *e )
{
    // Every cached pixel position was in the old geometry's units.
    lastPaintedPeakPx_ = lastPaintedRmsPx_ = lastPaintedHoldPx_ = -1;
    QWidget::resizeEvent( e );
}

QString SLevelMeter::describe() const
{
    return QStringLiteral( "vis=%1|orient=%2|len=%3|peak=%4|rms=%5|hold=%6|clip=%7|db=%8" )
        // !isHidden(), NOT isVisible(): isVisible() is false for any widget whose
        // ancestors are unshown, and the headless assertions build a head that is
        // never shown. What is being described is whether the DENSITY RULE hid the
        // meter, which is exactly what isHidden() answers.
        .arg( isHidden() ? 0 : 1 )
        .arg( orientation_ == Qt::Vertical ? QStringLiteral( "v" )
                                          : QStringLiteral( "h" ) )
        .arg( barLength() )
        .arg( dbToPx( ballistics_.peakDb() ) )
        .arg( dbToPx( ballistics_.rmsDb() ) )
        .arg( dbToPx( ballistics_.holdDb() ) )
        .arg( ballistics_.clipped() ? 1 : 0 )
        .arg( ballistics_.peakDb(), 0, 'f', 1 );
}
