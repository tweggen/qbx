
#ifndef _TWGRAINPARAMS_H_
#define _TWGRAINPARAMS_H_

#include <vector>

#include "tw/graph/twcomponent.h"   // length_t
#include "tw/core/twfraction.h"
#include "tw/core/twwarpmap.h"

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

    // Proposal 28 W1: user warp anchors — SANITIZED exact integer pairs
    // (source frames, warped frames), strictly increasing in both domains
    // (twWarpMap::sanitize is the gatekeeper at deserialize/edit time).
    // Empty = the scalar stretch everywhere (the historic behavior,
    // bit-identical). Non-empty forces the vocoder backend: piecewise maps
    // are a vocoder capability, not a Rubber Band one.
    std::vector<twWarpAnchor> warpAnchors;

    // Proposal 28 W4: opt-in formant preservation for the vocoder's pitch
    // stage. Default OFF (the proposal-26 measurement: preservation colours
    // and de-energises general material); a per-clip choice for vocal /
    // formant-bearing content. Ignored by the non-vocoder backends and a
    // strict no-op when pitchCents == 0.
    bool preserveFormants = false;

    // Formant shift, in cents, INDEPENDENT of pitchCents/stretch: a spectral-
    // envelope warp applied by the vocoder backend regardless of whether the
    // pitch stage runs. Default 0 = no-op (byte-identical output; ignored by
    // the non-vocoder backend). Positive raises the formants (brighter/
    // smaller-sounding), negative lowers them (darker/larger-sounding). See
    // twPagedVocoder::Config::formantRatio for the derivation.
    double formantShiftCents = 0.0;

    // True when the transform is a no-op, so callers can skip the grain stage
    // entirely (passthrough) and pay nothing for an unstretched clip.
    bool isIdentity() const {
        return stretch == Fraction(1) && pitchCents == 0.0
            && formantShiftCents == 0.0
            && warpAnchors.empty();
    }
};

#endif
