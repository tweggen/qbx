
#ifndef _TWGRAINPARAMS_H_
#define _TWGRAINPARAMS_H_

#include "tw/graph/twcomponent.h"   // length_t
#include "tw/core/twfraction.h"

/**
 * Parameters controlling grain-based time-stretch / pitch-shift (proposal 06).
 *
 * The first implementation is constant-rate time-slice overlap-add: fixed grain
 * size, fixed crossfade, one stretch factor and one pitch offset for the whole
 * clip. A pluggable slicer and a variable (automated) time map come later.
 *
 * The stretch factor is an EXACT rational (proposal 18 Phase 2): it is born
 * as a ratio of two integer frame counts (the stretch gesture's
 * newDuration/srcSpan), composes exactly under nesting and re-stretching,
 * and serializes as "n/d". Producers cap the denominator at CREATION time
 * (Fraction::limitedTo) — never during arithmetic.
 */
struct twGrainParams
{
    length_t grainSize  = 2048;   // frames per grain
    length_t crossfade  = 512;    // frames of linear crossfade at each grain edge
    Fraction stretch    = Fraction(1);  // output/input duration ratio (>1 = longer/slower)
    double   pitchCents = 0.0;    // pitch offset in cents (0 = unchanged)

    // True when the transform is a no-op, so callers can skip the grain stage
    // entirely (passthrough) and pay nothing for an unstretched clip.
    bool isIdentity() const {
        return stretch == Fraction(1) && pitchCents == 0.0;
    }
};

#endif
