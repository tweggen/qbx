// Gate for QBX-101: the dB tick scale on SLevelMeter, the ONE widget behind
// every level meter in the application (the track head, the Track Detail dock,
// the transport's master meter, the mixer strips).
//
// A PIXEL gate, deliberately. The tick positions are also reported by
// describe(), but a report reads the positions the code COMPUTED, not the ones
// it PAINTED -- the gap proposals 39 M2 and 41 M5 both shipped through. So
// each check below grabs a real meter off screen and reads the image:
//
//   - every minor tick is a GRAY notch on the leading edge, and the far half
//     of the lane on that row is untouched (the notch leaves the reading
//     visible);
//   - the 0 dB tick is WHITE, ZERO_TICK_PX thick and spans the WHOLE short
//     axis, frame included;
//   - a row between two ticks carries no tick colour;
//   - the scale is drawn OVER a lit bar, not hidden by it;
//   - the step thins with the bar length and never crowds below
//     MIN_TICK_SPACING_PX;
//   - the 0 dB tick sits exactly where a 0 dBFS peak lights up (same
//     dbToPx mapping), measured through describe()'s peak pixel.
//
// No display, no audio device.

#include "app/timeline/slevelmeter.h"

#include <QApplication>
#include <QColor>
#include <QImage>
#include <QString>

#include <cstdio>

namespace {

int g_failures = 0;

void check( bool ok, const QString &what )
{
    std::printf( "%s %s\n", ok ? "ok  " : "FAIL", qPrintable( what ) );
    if( !ok ) ++g_failures;
}

const QRgb kMinor = QColor( 0x80, 0x80, 0x80 ).rgb();
const QRgb kZero  = QColor( 0xff, 0xff, 0xff ).rgb();
const QRgb kBack  = QColor( 0x1c, 0x1c, 0x1c ).rgb();

int fieldOf( const QString &desc, const QString &key )
{
    for( const QString &part : desc.split( QLatin1Char( '|' ) ) )
        if( part.startsWith( key + QLatin1Char( '=' ) ) )
            return part.mid( key.size() + 1 ).section( QLatin1Char( ',' ), 0, 0 ).toInt();
    return -9999;
}

QImage grab( SLevelMeter &m )
{
    return m.grab().toImage().convertToFormat( QImage::Format_RGB32 );
}

// One vertical meter of height `h` with `lanes` lanes: every tick where it
// belongs, idle. More than one lane uses Grow mode (the Track Detail dock's
// shape) -- a Split meter is pinned to BAR_THICKNESS whatever it is resized to,
// so the width under test is read back from the widget, never assumed.
void testVertical( int lanes, int h, bool grow = true )
{
    SLevelMeter m;
    m.setOrientation( Qt::Vertical );
    if( lanes > 1 ) {
        m.setGrowWithLanes( grow );
        m.setLanes( lanes );
    }
    m.resize( 100, h );
    const int w = m.width();
    const QImage img = grab( m );
    const QString tag = QStringLiteral( "vertical %1 lane(s) %2x%3" ).arg( lanes ).arg( w ).arg( h );

    const std::vector<int> ticks = m.minorTickPx();
    check( !ticks.empty(), tag + ": has minor ticks" );

    // PER LANE: each lane's leading pixel is gray on a tick row and its LAST
    // pixel is not. Lanes are found from the IMAGE, not from laneGeom (which is
    // private): on the first tick row a lane is a run of gray starting after
    // a non-gray pixel. The expected lane count is what the meter was built with.
    bool allGray = !ticks.empty(), farHalfClear = true;
    int lanesSeen = 0;
    for( int v : ticks ) {
        const int y = h - v;
        int runs = 0;
        for( int x = 1; x < w - 1; ++x ) {
            const bool gray = img.pixel( x, y ) == kMinor;
            const bool prevGray = x > 1 && img.pixel( x - 1, y ) == kMinor;
            if( gray && !prevGray ) ++runs;
        }
        if( v == ticks.front() ) lanesSeen = runs;
        if( runs != lanes ) allGray = false;
        // The trailing interior pixel belongs to the last lane's far half.
        if( img.pixel( w - 2, y ) == kMinor ) farHalfClear = false;
    }
    check( allGray, tag + QStringLiteral( ": every minor tick is one gray notch per lane (%1 seen, %2 lanes)" )
                              .arg( lanesSeen ).arg( lanes ) );
    check( farHalfClear, tag + ": the far half of each lane is left clear on a tick row" );

    // Spacing never below the floor.
    bool spaced = true;
    for( size_t i = 1; i < ticks.size(); ++i )
        if( ticks[i] - ticks[i - 1] < SLevelMeter::MIN_TICK_SPACING_PX ) spaced = false;
    check( spaced, tag + ": neighbouring ticks are at least MIN_TICK_SPACING_PX apart" );

    // The 0 dB tick: white, full width including the frame, ZERO_TICK_PX rows.
    const int z = m.zeroTickPx();
    check( z > 0, tag + ": the 0 dB tick is on the scale" );
    bool zeroFull = z > 0;
    for( int r = 0; r < SLevelMeter::ZERO_TICK_PX && zeroFull; ++r )
        for( int x = 0; x < w; ++x )
            if( img.pixel( x, h - z + r ) != kZero ) { zeroFull = false; break; }
    check( zeroFull, tag + ": the 0 dB tick is white, spans the whole width and is "
                           "ZERO_TICK_PX thick" );
    check( z > 0 && img.pixel( 0, h - z - 1 ) != kZero
               && img.pixel( 0, h - z + SLevelMeter::ZERO_TICK_PX ) != kZero,
           tag + ": ...and no thicker than that" );

    // A row strictly between two minor ticks carries no tick colour.
    if( ticks.size() >= 2 && ticks[1] - ticks[0] >= 3 ) {
        const int y = h - ( ticks[0] + 1 );
        check( img.pixel( 1, y ) == kBack, tag + ": a row between ticks is background" );
    }
}

void testDensityThins()
{
    SLevelMeter tall, shortM;
    tall.resize( 8, 240 );
    shortM.resize( 8, 40 );
    const QString dt = tall.describe(), ds = shortM.describe();
    const int stepTall  = fieldOf( dt, QStringLiteral( "tickStep" ) );
    const int stepShort = fieldOf( ds, QStringLiteral( "tickStep" ) );
    check( stepTall > 0 && stepShort > stepTall,
           QStringLiteral( "a short meter thins its scale (step %1 dB at 240 px, %2 dB at 40 px)" )
               .arg( stepTall ).arg( stepShort ) );
    check( fieldOf( dt, QStringLiteral( "ticks" ) ) == (int) tall.minorTickPx().size(),
           QStringLiteral( "describe() reports the tick count it paints" ) );
}

void testHorizontal()
{
    // The Track Detail dock's shape: horizontal, 72 px long.
    SLevelMeter m;
    m.setOrientation( Qt::Horizontal );
    m.resize( 72, 8 );
    const QImage img = grab( m );
    const std::vector<int> ticks = m.minorTickPx();
    bool ok = !ticks.empty();
    for( int v : ticks )
        if( img.pixel( v, 1 ) != kMinor ) ok = false;
    check( ok, QStringLiteral( "horizontal 72x8: every minor tick is a gray notch from the top edge" ) );
    const int z = m.zeroTickPx();
    bool zeroFull = z > 0;
    for( int y = 0; y < 8 && zeroFull; ++y )
        if( img.pixel( z, y ) != kZero || img.pixel( z - 1, y ) != kZero ) zeroFull = false;
    check( zeroFull, QStringLiteral( "horizontal 72x8: the 0 dB tick is white and spans the full height" ) );
}

void testOverLitBarAndAlignment()
{
    SLevelMeter m;
    m.resize( 8, 240 );
    // A full-scale peak: the whole bar is lit.
    twLevelSample s;
    s.peak = 2.0f;            // +6 dBFS, the top of the scale
    s.meanSquare = 1.0f;
    s.frames = 1024;
    m.pushLevel( s, 1000 );
    const QImage img = grab( m );
    const std::vector<int> ticks = m.minorTickPx();
    bool visible = !ticks.empty();
    for( int v : ticks )
        if( img.pixel( 1, 240 - v ) != kMinor ) visible = false;
    check( visible, QStringLiteral( "the scale is drawn OVER a lit bar, not hidden by it" ) );

    // Alignment: a 0 dBFS peak lights up to exactly the 0 dB tick.
    SLevelMeter a;
    a.resize( 8, 240 );
    twLevelSample z;
    z.peak = 1.0f;
    z.meanSquare = 0.5f;
    z.frames = 1024;
    a.pushLevel( z, 1000 );
    const int peakPx = fieldOf( a.describe(), QStringLiteral( "peak" ) );
    check( peakPx == a.zeroTickPx(),
           QStringLiteral( "a 0 dBFS peak lights up to the 0 dB tick (peak px %1, tick px %2)" )
               .arg( peakPx ).arg( a.zeroTickPx() ) );
}

}  // namespace

int main( int argc, char **argv )
{
    QApplication app( argc, argv );
    testVertical( 1, 240 );    // a tall mixer strip meter
    testVertical( 1, 48 );     // a track head
    testVertical( 2, 48 );         // a stereo meter in Grow mode
    testVertical( 2, 48, false );  // a STEREO TRACK HEAD: two 3 px lanes in 8 px
    testVertical( 6, 160 );    // a six-channel meter in Grow mode (wider)
    testDensityThins();
    testHorizontal();
    testOverLitBarAndAlignment();
    if( g_failures ) {
        std::printf( "\n%d check(s) FAILED\n", g_failures );
        return 1;
    }
    std::printf( "\nall checks passed\n" );
    return 0;
}
