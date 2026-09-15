#ifndef _TWPANLAW_H
#define _TWPANLAW_H

#include <cmath>

/**
 * THE PAN LAW (proposal 49 D1). One pure function, used by every stage that
 * pans: a clip in twTrackMix, a track in twGainStage, the live pump.
 *
 *     p in [-1, 1]      (the stored SObject::pan_ unit)
 *
 *     p == 0    :  l = 1,             r = 1              exactly, no arithmetic
 *     p  > 0    :  l = cos(p * pi/2), r = 1              the far side falls
 *     p  < 0    :  l = 1,             r = cos(-p * pi/2)
 *     |p| >= 1  :  the far side is EXACTLY 0.0           not cos(pi/2) = 6.1e-17
 *
 * Applied per OUTPUT channel: channel 0 x l, channel 1 x r.
 *
 * CENTRE-UNITY IS FORCED, NOT CHOSEN. Every existing project was mixed with
 * pan at centre = unity, because pan did nothing. A -3 dB centre would make
 * every project quieter the day pan became audible and break byte identity for
 * every golden. Among centre-unity laws this is the BALANCE family (the near
 * side stays at unity), not the compensated one (the near side rises to
 * +3 dB), because a pan gesture must never be able to clip a track.
 *
 * DEFINED FOR WIDTH 2 ONLY. At any other project width a stage passes audio
 * through and the verbs refuse a non-zero pan (D1). This function does not
 * know the width; the CALLER decides whether to call it.
 *
 * A NaN pan is treated as centre, and a value outside [-1, 1] as hard pan, so
 * no input can produce a gain above 1 or a NaN sample.
 */
struct twPanGains
{
    double l;
    double r;
};

inline twPanGains twPanLaw( double p )
{
    if( p == 0.0 || std::isnan( p ) )
        return { 1.0, 1.0 };

    static constexpr double kHalfPi = 1.5707963267948966192313216916397514;
    if( p > 0.0 )
        return { ( p >= 1.0 ) ? 0.0 : std::cos( p * kHalfPi ), 1.0 };
    return { 1.0, ( p <= -1.0 ) ? 0.0 : std::cos( -p * kHalfPi ) };
}

#endif
