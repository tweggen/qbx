#ifndef _SFADERCURVE_H
#define _SFADERCURVE_H

// The ONE volume-fader mapping, shared by every fader in the app.
//
// It lives in a header because there is more than one fader onto the same track
// volume — the arranger track head (SSMVMixerControl) and the Track Detail dock
// (STrackDetailPanel) — and they used to disagree: the dock did the naive
// `value = dB * 10`, so the same 0 dB sat at two different pixel positions and
// dragging one moved the other somewhere else. A fader curve is a property of
// the control, not of a particular widget.

#include <QtGlobal>
#include <cmath>

// The fader works in tenths of a dB so it can drive an integer QSlider:
// slider value v <-> v/10 dB nominal. Range -96.0 .. +24.0 dB.
static const int    SFADER_MIN    = -960;
static const int    SFADER_MAX    =  240;
static const double SFADER_MIN_DB =  -96.0;
static const double SFADER_MAX_DB =   24.0;

// Fader curve exponent: y = x^n over the normalized [0,1] travel. n=1 is linear;
// n<1 spends more travel on the loud end, which is where fader precision matters.
static const double SFADER_CURVE_EXPONENT = 0.5;

inline double sFaderToDb( int sliderValue )
{
    double normalized = (double)( sliderValue - SFADER_MIN )
                      / (double)( SFADER_MAX - SFADER_MIN );
    normalized = qBound( 0.0, normalized, 1.0 );
    const double curved = std::pow( normalized, SFADER_CURVE_EXPONENT );
    return SFADER_MIN_DB + curved * ( SFADER_MAX_DB - SFADER_MIN_DB );
}

inline int sDbToFader( double dB )
{
    double normalized = ( dB - SFADER_MIN_DB ) / ( SFADER_MAX_DB - SFADER_MIN_DB );
    normalized = qBound( 0.0, normalized, 1.0 );
    const double curved = std::pow( normalized, 1.0 / SFADER_CURVE_EXPONENT );
    return (int)( SFADER_MIN + curved * ( SFADER_MAX - SFADER_MIN ) + 0.5 );
}


// ONE WHEEL NOTCH IS ONE dB (proposal 48 AC4.4), and the arithmetic HAS to
// happen in dB rather than in slider units.
//
// The curve is `y = x^0.5` over the travel, so a slider unit is NOT a tenth of
// a dB anywhere except nominally: near 0 dB one dB is ~16 units, near -60 dB
// it is ~6. `setSingleStep( 10 )` therefore never meant "1 dB per notch" --
// and QAbstractSlider multiplies the step by QApplication::wheelScrollLines()
// (3 by default) on top, so a notch on the arranger head was ~1.9 dB at unity
// and ~5 dB down at -60. The comment in `SSMVMixerControl::wheelEvent` claimed
// 1.0 dB per notch and had been wrong since it was written; this is the
// function that makes it true, in BOTH mounts.
//
// THE STALL GUARD IS NOT DECORATION. The bottom of the range is compressed
// past the integer slider's resolution -- -96 dB and -95 dB round to the SAME
// value (-960) -- so a pure dB round trip would leave a notch there moving
// nothing at all and read as a dead control. One unit is then the honest
// minimum: it is the smallest change this slider can express.
static const double SFADER_WHEEL_DB = 1.0;

inline int sFaderWheelValue( int cur, int notches )
{
    if( notches == 0 ) return cur;
    const double db = qBound( SFADER_MIN_DB,
                              sFaderToDb( cur ) + notches * SFADER_WHEEL_DB,
                              SFADER_MAX_DB );
    int next = sDbToFader( db );
    if( next == cur ) next = cur + ( notches > 0 ? 1 : -1 );
    return qBound( SFADER_MIN, next, SFADER_MAX );
}

#endif
