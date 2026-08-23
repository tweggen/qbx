#ifndef _TW_FADE_H_
#define _TW_FADE_H_

#include <cstdint>
#include <cmath>

/**
 * twClipFade — a clip's FADE-IN and FADE-OUT, as a value (proposal 43 N5).
 *
 * There was no fade primitive anywhere in this model before this. The only
 * things that looked like one are not: `twGrainParams::crossfadeMs` is a
 * granular-synthesis knob, and the live monitor's 2-3 ms ramp is an RT detail
 * of the device lane.
 *
 * Read BY POSITION in the clip's OWN domain, exactly as `twTrackMix` already
 * reads a `cut:Gain` curve — so a fade trims, slips and loops with its clip,
 * and composes with the clip's gain curve and static volume as a per-frame
 * PRODUCT of linear factors (mix/CONTRACT.md inv. 21's "TRIM SUMS IN dB").
 *
 * THE FADE-OUT IS ANCHORED TO THE CLIP'S END, which is why `gainAt` takes the
 * clip's length rather than the fade storing one: a clip's length changes
 * under a trim, and a fade-out that did not follow it would slide into the
 * middle of the clip on the first resize.
 *
 * The SHAPE is shared with the crossfade N3 will build, deliberately: a
 * crossfade is two fades that meet, and two curve families that could disagree
 * about the shape of the same join is one more than there should be.
 */

enum class twFadeShape : uint8_t {
    Linear = 0,      // gain = t
    EqualPower = 1   // gain = sin(t * pi/2) -- two of these SUM to constant
                     // power across a join, which is what a crossfade wants
};

struct twClipFade {
    int64_t     inLen  = 0;                     // frames from the clip START
    int64_t     outLen = 0;                     // frames ending at the clip END
    twFadeShape shape  = twFadeShape::Linear;

    bool none() const { return inLen <= 0 && outLen <= 0; }

    bool operator==( const twClipFade &o ) const {
        return inLen == o.inLen && outLen == o.outLen && shape == o.shape;
    }
    bool operator!=( const twClipFade &o ) const { return !( *this == o ); }

    /** The fade's linear gain at `pos`, in a clip of `clipLen` frames. */
    double gainAt( int64_t pos, int64_t clipLen ) const
    {
        double g = 1.0;
        if( inLen > 0 && pos < inLen )
            g *= shaped( (double) pos / (double) inLen, shape );
        if( outLen > 0 && clipLen > 0 ) {
            const int64_t outStart = clipLen - outLen;
            if( pos >= outStart )
                g *= shaped( (double) ( clipLen - pos ) / (double) outLen,
                             shape );
        }
        return g;
    }

    /**
     * A fade IN and a fade OUT that overlap (a clip shorter than the sum) both
     * apply, as a product. That is the honest reading of "both are declared",
     * it degrades continuously as the clip is trimmed, and it is what makes a
     * very short clip fade to a peak below unity rather than jump.
     */
    static double shaped( double t, twFadeShape s = twFadeShape::Linear )
    {
        if( t <= 0.0 ) return 0.0;
        if( t >= 1.0 ) return 1.0;
        return ( s == twFadeShape::EqualPower )
                   ? std::sin( t * 1.5707963267948966 )   // pi/2
                   : t;
    }
};

#endif // _TW_FADE_H_
