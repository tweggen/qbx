// Gate for proposal 49 M0 (AC0.2): the ONE pan-control spelling,
// app/timeline/spanscale.h. Every pan control in the app (arranger head, Track
// Detail dock, mixer strip, Clip Properties) is to be a mount of it, so a
// disagreement between two of them can only be introduced here -- and is
// caught here.
//
// No widget, no display: the header is pure arithmetic plus a QString.

#include "app/timeline/spanscale.h"

#include <QString>

#include <cstdio>
#include <limits>

namespace {

int g_failures = 0;

void check( bool ok, const QString &what )
{
    std::printf( "%s %s\n", ok ? "ok  " : "FAIL", qPrintable( what ) );
    if( !ok ) ++g_failures;
}

} // namespace

int main()
{
    // --- tick <-> value round trip, EXACT over the whole travel ---------------
    {
        bool tickTrip = true, valueTrip = true;
        QString firstBad;
        for( int t = SPAN_TICK_MIN; t <= SPAN_TICK_MAX; ++t ) {
            const double p = sTickToPan( t );
            if( sPanToTick( p ) != t ) {
                tickTrip = false;
                if( firstBad.isEmpty() ) firstBad = QString::number( t );
            }
            if( p != t / 100.0 ) valueTrip = false;
        }
        check( tickTrip, QStringLiteral( "sPanToTick(sTickToPan(t)) == t for every t in -100..100%1" )
                             .arg( firstBad.isEmpty() ? QString() : QStringLiteral( " (first bad %1)" ).arg( firstBad ) ) );
        check( valueTrip, QStringLiteral( "sTickToPan(t) == t/100 exactly (the stored value a control commits)" ) );
    }

    check( SPAN_TICK_MIN == -100 && SPAN_TICK_MAX == 100,
           QStringLiteral( "the travel is -100..100" ) );
    check( SPAN_DEFAULT == 0.0, QStringLiteral( "the reset value is exactly 0.0" ) );
    check( sTickToPan( 0 ) == 0.0 && sPanToTick( 0.0 ) == 0,
           QStringLiteral( "centre is tick 0 is value 0.0" ) );

    // --- clamps and hostile input ---------------------------------------------
    check( sTickToPan( 250 ) == 1.0 && sTickToPan( -999 ) == -1.0,
           QStringLiteral( "an out-of-range tick clamps to the end of travel" ) );
    check( sPanToTick( 1.7 ) == 100 && sPanToTick( -4.0 ) == -100,
           QStringLiteral( "an out-of-range value clamps to the end of travel" ) );
    check( sPanToTick( std::numeric_limits<double>::quiet_NaN() ) == 0,
           QStringLiteral( "a NaN value is centre" ) );

    // --- text --------------------------------------------------------------------
    {
        bool allText = true;
        QString firstBad;
        for( int t = SPAN_TICK_MIN; t <= SPAN_TICK_MAX; ++t ) {
            const QString want = ( t == 0 ) ? QStringLiteral( "C" )
                               : ( t < 0 )  ? QStringLiteral( "L%1" ).arg( -t )
                                            : QStringLiteral( "R%1" ).arg( t );
            const QString got = sPanText( sTickToPan( t ) );
            if( got != want ) {
                allText = false;
                if( firstBad.isEmpty() ) firstBad = QStringLiteral( "%1 -> '%2' want '%3'" ).arg( t ).arg( got, want );
            }
        }
        check( allText, QStringLiteral( "text is C, L1..L100, R1..R100 over every tick%1" )
                            .arg( firstBad.isEmpty() ? QString() : QStringLiteral( " (%1)" ).arg( firstBad ) ) );
    }
    check( sPanText( 0.0 ) == QStringLiteral( "C" ), QStringLiteral( "sPanText(0) == \"C\"" ) );
    check( sPanText( -0.37 ) == QStringLiteral( "L37" ), QStringLiteral( "sPanText(-0.37) == \"L37\"" ) );
    check( sPanText( 1.0 ) == QStringLiteral( "R100" ), QStringLiteral( "sPanText(1) == \"R100\"" ) );
    check( sPanText( 0.004 ) == QStringLiteral( "C" ),
           QStringLiteral( "a value between ticks reads as the tick a control would show" ) );

    std::printf( "\n%s (%d failure%s)\n", g_failures ? "FAILED" : "PASSED", g_failures,
                 g_failures == 1 ? "" : "s" );
    return g_failures ? 1 : 0;
}
