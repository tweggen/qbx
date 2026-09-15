#ifndef _SPANSCALE_H
#define _SPANSCALE_H

// The ONE pan-control spelling, shared by every pan control in the app
// (proposal 49 D4) -- the sfadercurve.h precedent.
//
// There will be four mounts onto two pan values: the arranger track head, the
// Track Detail dock and the mixer strip onto a TRACK's pan, and the Clip
// Properties panel onto a CLIP's. The faders once disagreed about where 0 dB
// sat because each widget spelled its own mapping; a pan control is the same
// hazard, so the tick, the text and the default are spelled here once. The
// audible half of pan (the law) is tw/mix/twpanlaw.h and is not a UI concern.

#include <QString>
#include <QtGlobal>
#include <cmath>

// A pan control is an integer control over -100..100: one tick is 1 % of the
// travel toward one side. Unlike the fader curve this ROUND-TRIPS EXACTLY --
// sPanToTick( sTickToPan( t ) ) == t for every tick -- so a control never
// commits a value it does not show.
static const int    SPAN_TICK_MIN = -100;
static const int    SPAN_TICK_MAX =  100;

// The value a double-click reset commits. Committed DIRECTLY, never through a
// tick (the applyVolumeDb precedent), so a reset lands on exactly 0.0 -- the
// value that does no arithmetic at all (proposal 49 D5).
static const double SPAN_DEFAULT  =    0.0;

inline double sTickToPan( int tick )
{
    return (double)qBound( SPAN_TICK_MIN, tick, SPAN_TICK_MAX ) / 100.0;
}

// NaN is centre and out-of-range values clamp, matching twPanLaw.
inline int sPanToTick( double pan )
{
    if( std::isnan( pan ) )
        return 0;
    const double scaled = qBound( -1.0, pan, 1.0 ) * 100.0;
    return (int)std::lround( scaled );
}

// "C" at centre, "L37" / "R100" otherwise. The same text in every mount, in
// every tooltip and in every describe(). It is spelled from the TICK, so a
// stored value between two ticks reads as the tick a control would show.
inline QString sPanText( double pan )
{
    const int tick = sPanToTick( pan );
    if( tick == 0 )
        return QStringLiteral( "C" );
    return ( tick < 0 ? QStringLiteral( "L" ) : QStringLiteral( "R" ) )
         + QString::number( tick < 0 ? -tick : tick );
}

#endif
