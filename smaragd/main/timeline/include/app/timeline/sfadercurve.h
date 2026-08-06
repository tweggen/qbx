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

#endif
