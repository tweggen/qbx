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
    ballistics_.resize( 1 );
    lastPainted_.resize( 1 );
    updateToolTip_();
    setOrientation( Qt::Vertical );
}

void SLevelMeter::setLanes( int shown, int available )
{
    if( shown < 1 ) shown = 1;
    if( shown > twLevelSampleSet::MAX_LANES ) shown = twLevelSampleSet::MAX_LANES;
    if( available < shown ) available = shown;

    if( (int) ballistics_.size() == shown && available_ == available ) return;

    // Resizing DOWN drops lanes and resizing UP adds fresh ones at the floor;
    // surviving lanes keep their state, so a width change does not make every
    // bar jump.
    ballistics_.resize( (size_t) shown );
    lastPainted_.assign( (size_t) shown, LanePx() );
    available_ = available;

    updateToolTip_();
    applySizeConstraints_();
    updateGeometry();
    update();
}

void SLevelMeter::setGrowWithLanes( bool grow )
{
    if( grow_ == grow ) return;
    grow_ = grow;
    lastPainted_.assign( ballistics_.size(), LanePx() );
    applySizeConstraints_();
    updateGeometry();
    update();
}

void SLevelMeter::setMeterLabel( const QString &label )
{
    if( label_ == label ) return;
    label_ = label;
    updateToolTip_();
}

void SLevelMeter::updateToolTip_()
{
    QString t = ( label_.isEmpty() ? QStringLiteral( "Level (peak + RMS)" ) : label_ )
                + QStringLiteral( " — click to clear the clip indicator" );
    const int shown = (int) ballistics_.size();
    if( available_ > shown ) {
        // The one thing a user must be able to find out: WHY there are two bars
        // on a six-channel project. Same rule the monitor path states
        // (twSpeaker: L = ch0, R = ch1, the rest computed and dropped), and the
        // dock is named because that is where the answer stops being a cap.
        t += QStringLiteral(
                 "\nShowing channels 1–%1 of %2, like the monitor path.\n"
                 "The Track Detail dock meters every channel." )
                 .arg( shown ).arg( available_ );
    } else if( shown > 1 ) {
        t += QStringLiteral( "\n%1 channels, lane 1 leftmost/lowest." ).arg( shown );
    }
    setToolTip( t );
}

int SLevelMeter::thickness() const
{
    const int n = (int) ballistics_.size();
    if( !grow_ ) return BAR_THICKNESS;
    // frame (1 px each side) + n lanes + (n-1) separators. n == 1 gives
    // 2 + 5 + 0 = 7, so keep BAR_THICKNESS as the floor: a mono dock meter must
    // look exactly like it did before B8.
    const int want = 2 + n * GROW_LANE_THICKNESS + ( n - 1 );
    return want > BAR_THICKNESS ? want : BAR_THICKNESS;
}

void SLevelMeter::applySizeConstraints_()
{
    const int t = thickness();
    // Set BOTH axes every time: setFixedWidth pins min == max, so flipping
    // orientation without clearing the other axis leaves the bar stuck at its
    // previous thickness.
    if( orientation_ == Qt::Vertical ) {
        setMinimumWidth( t );
        setMaximumWidth( t );
        setMinimumHeight( 24 );
        setMaximumHeight( QWIDGETSIZE_MAX );
    } else {
        setMinimumHeight( t );
        setMaximumHeight( t );
        setMinimumWidth( 24 );
        setMaximumWidth( QWIDGETSIZE_MAX );
    }
}

void SLevelMeter::setOrientation( Qt::Orientation o )
{
    orientation_ = o;
    applySizeConstraints_();
    // Geometry changed, so the cached pixel state describes a different bar.
    lastPainted_.assign( ballistics_.size(), LanePx() );
    updateGeometry();
    update();
}

int SLevelMeter::barLength() const
{
    // One pixel of frame at each end.
    const int raw = ( orientation_ == Qt::Vertical ? height() : width() ) - 2;
    return raw > 0 ? raw : 0;
}

bool SLevelMeter::laneGeom( int i, int &offset, int &extent ) const
{
    const int n = (int) ballistics_.size();
    if( i < 0 || i >= n ) return false;

    // Interior of the short axis, i.e. inside the 1 px frame. Use the widget's
    // ACTUAL size, not thickness(): a layout may hand it something else, and
    // painting outside what was given is the "squashed head" symptom.
    const int interior = ( orientation_ == Qt::Vertical ? width() : height() ) - 2;
    if( interior < 1 ) return false;

    // A 1 px separator between lanes, but only while it leaves each lane at
    // least 2 px — below that the separator costs more than it explains.
    int gap = 0;
    if( n > 1 && interior >= 3 * n ) gap = 1;
    int lane = ( interior - gap * ( n - 1 ) ) / n;
    if( lane < 1 ) { lane = 1; gap = 0; }

    const int used = n * lane + gap * ( n - 1 );
    const int pad  = ( interior - used ) / 2;       // centre the block
    offset = 1 + ( pad > 0 ? pad : 0 ) + i * ( lane + gap );
    extent = lane;
    // Never draw past the widget.
    const int limit = 1 + interior;
    if( offset >= limit ) return false;
    if( offset + extent > limit ) extent = limit - offset;
    return extent >= 1;
}

int SLevelMeter::dbToPx( float db ) const
{
    const twMeterBallisticsConfig &cfg = ballistics_[0].config();
    const int len = barLength();
    if( len <= 0 ) return 0;

    const float span = cfg.ceilDb - cfg.floorDb;
    if( !( span > 0.0f ) ) return 0;

    float t = ( db - cfg.floorDb ) / span;      // dB-linear, so the scale reads
    if( t <= 0.0f ) return 0;                   // evenly in dB
    if( t >= 1.0f ) return len;
    return (int) ( t * (float) len + 0.5f );
}

void SLevelMeter::pushLevel( const twLevelSampleSet &s, qint64 nowMs )
{
    const double now = (double) nowMs / 1000.0;
    for( size_t i = 0; i < ballistics_.size(); ++i ) {
        if( (int) i < s.lanes ) ballistics_[i].push( s.lane[i], now );
        else                    ballistics_[i].idle( now );
    }
    refresh_();
}

void SLevelMeter::pushLevel( const twLevelSample &s, qint64 nowMs )
{
    twLevelSampleSet set;
    set.lane[0] = s;
    set.lanes   = 1;
    pushLevel( set, nowMs );
}

void SLevelMeter::pushIdle( qint64 nowMs )
{
    const double now = (double) nowMs / 1000.0;
    for( twMeterBallistics &b : ballistics_ ) b.idle( now );
    refresh_();
}

void SLevelMeter::resetMeter()
{
    for( twMeterBallistics &b : ballistics_ ) b.reset();
    refresh_();
}

void SLevelMeter::refresh_()
{
    const int n = (int) ballistics_.size();

    // The single-lane path keeps proposal 34's sub-rect update exactly: it is
    // the common case (every track head on a mono or stereo project) and it is
    // what makes 30 heads at 30 Hz cost nothing.
    if( n == 1 ) {
        const int  peakPx = dbToPx( ballistics_[0].peakDb() );
        const int  rmsPx  = dbToPx( ballistics_[0].rmsDb() );
        const int  holdPx = dbToPx( ballistics_[0].holdDb() );
        const bool clip   = ballistics_[0].clipped();
        const LanePx &was = lastPainted_[0];

        if( peakPx == was.peak && rmsPx == was.rms &&
            holdPx == was.hold && clip == was.clip ) {
            return;                       // nothing would change on screen
        }

        // A clip cap appearing or clearing changes the far end of the bar;
        // anything else moves within the band between the old and new positions.
        if( clip != was.clip || was.peak < 0 ) {
            update();
            return;
        }

        int lo = qMin( qMin( peakPx, was.peak ),
                       qMin( qMin( rmsPx, was.rms ),
                             qMin( holdPx, was.hold ) ) );
        int hi = qMax( qMax( peakPx, was.peak ),
                       qMax( qMax( rmsPx, was.rms ),
                             qMax( holdPx, was.hold ) ) );
        lo -= 2; hi += 2;                  // the hold tick is 2 px tall

        if( orientation_ == Qt::Vertical ) {
            // The bar grows upward from the bottom, so a larger px is a smaller y.
            const int yTop = height() - 1 - hi;
            update( 0, yTop, width(), hi - lo + 1 );
        } else {
            update( 1 + lo, 0, hi - lo + 1, height() );
        }
        return;
    }

    // Multi-lane: the lanes move independently across the short axis as well as
    // along the growth axis, so the band trick would have to become a union of
    // per-lane rects for no measurable gain on the handful of multi-lane meters
    // that exist (one dock, one master, one per head). Repaint the widget, but
    // still only when something WOULD change.
    for( int i = 0; i < n; ++i ) {
        const twMeterBallistics &b = ballistics_[(size_t) i];
        const LanePx &was = lastPainted_[(size_t) i];
        if( dbToPx( b.peakDb() ) != was.peak || dbToPx( b.rmsDb() )  != was.rms ||
            dbToPx( b.holdDb() ) != was.hold || b.clipped()          != was.clip ) {
            update();
            return;
        }
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

    const int amberAt = dbToPx( kAmberFromDb );
    const int redAt   = dbToPx( kRedFromDb );

    for( int i = 0; i < (int) ballistics_.size(); ++i ) {
        int laneOff = 0, laneExt = 0;
        if( !laneGeom( i, laneOff, laneExt ) ) continue;

        const twMeterBallistics &b = ballistics_[(size_t) i];
        const int peakPx = dbToPx( b.peakDb() );
        const int rmsPx  = dbToPx( b.rmsDb() );
        const int holdPx = dbToPx( b.holdDb() );
        const bool clip  = b.clipped();

        // Paint the lit part in up to three zone-coloured slabs rather than
        // per-pixel: the colour depends only on where a pixel sits on the scale.
        const struct { int from, to; const QColor &c; } zones[3] = {
            { 0,       qMin( peakPx, amberAt ), kZoneGreen },
            { amberAt, qMin( peakPx, redAt ),   kZoneAmber },
            { redAt,   peakPx,                  kZoneRed   },
        };

        auto slab = [&]( int from, int to, const QColor &c, int inset ) {
            if( to <= from ) return;
            const int off = laneOff + inset;
            const int ext = laneExt - 2 * inset;
            if( ext <= 0 ) return;
            if( orientation_ == Qt::Vertical ) {
                const int yBottom = height() - 1 - from;
                p.fillRect( off, yBottom - ( to - from ) + 1, ext, to - from, c );
            } else {
                p.fillRect( 1 + from, off, to - from, ext, c );
            }
        };

        for( const auto &z : zones ) slab( z.from, z.to, z.c, 0 );

        // The RMS bar sits INSIDE the peak bar: same zone colours showing
        // through, brightened by a translucent overlay, so one lane carries both
        // readings. A lane too thin to inset gets the tint across its whole
        // width instead of losing the RMS reading altogether.
        const int inset = laneExt >= 4 ? laneExt / 4 : 0;
        if( rmsPx > 0 ) slab( 0, rmsPx, kRmsTint, inset );

        // Held peak tick.
        if( holdPx > 0 ) slab( qMax( 0, holdPx - 2 ), holdPx, kHoldTick, 0 );

        // Clip cap at the far end — latched until clicked, never time-based.
        if( clip ) slab( qMax( 0, len - 2 ), len, kClipCap, 0 );

        LanePx &was = lastPainted_[(size_t) i];
        was.peak = peakPx;
        was.rms  = rmsPx;
        was.hold = holdPx;
        was.clip = clip;
    }
}

void SLevelMeter::mousePressEvent( QMouseEvent *e )
{
    if( e->button() == Qt::LeftButton ) {
        bool any = false;
        for( twMeterBallistics &b : ballistics_ ) {
            if( b.clipped() ) { b.clearClip(); any = true; }
        }
        if( any ) {
            // One click clears every lane: the clip indicator is one statement
            // about this track, and clearing three of six bars would be a worse
            // reading than clearing none.
            refresh_();
            e->accept();
            return;
        }
    }
    QWidget::mousePressEvent( e );
}

void SLevelMeter::resizeEvent( QResizeEvent *e )
{
    // Every cached pixel position was in the old geometry's units.
    lastPainted_.assign( ballistics_.size(), LanePx() );
    QWidget::resizeEvent( e );
}

QString SLevelMeter::describe() const
{
    QString peaks, rmss, holds, dbs;
    for( size_t i = 0; i < ballistics_.size(); ++i ) {
        if( i ) { peaks += ','; rmss += ','; holds += ','; dbs += ','; }
        peaks += QString::number( dbToPx( ballistics_[i].peakDb() ) );
        rmss  += QString::number( dbToPx( ballistics_[i].rmsDb() ) );
        holds += QString::number( dbToPx( ballistics_[i].holdDb() ) );
        dbs   += QString::number( ballistics_[i].peakDb(), 'f', 1 );
    }

    bool anyClip = false;
    for( const twMeterBallistics &b : ballistics_ ) if( b.clipped() ) anyClip = true;

    return QStringLiteral(
               "vis=%1|orient=%2|lanes=%3|width=%4|len=%5|peak=%6|rms=%7|hold=%8|clip=%9|db=%10" )
        // !isHidden(), NOT isVisible(): isVisible() is false for any widget whose
        // ancestors are unshown, and the headless assertions build a head that is
        // never shown. What is being described is whether the DENSITY RULE hid the
        // meter, which is exactly what isHidden() answers.
        .arg( isHidden() ? 0 : 1 )
        .arg( orientation_ == Qt::Vertical ? QStringLiteral( "v" )
                                          : QStringLiteral( "h" ) )
        .arg( (int) ballistics_.size() )
        .arg( available_ )
        .arg( barLength() )
        .arg( peaks )
        .arg( rmss )
        .arg( holds )
        .arg( anyClip ? 1 : 0 )
        .arg( dbs );
}
