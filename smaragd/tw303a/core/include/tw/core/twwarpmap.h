
#ifndef _TWWARPMAP_H_
#define _TWWARPMAP_H_

#include <cstdint>
#include <vector>

#include "tw/core/twfraction.h"

/**
 * twWarpMap — THE conversion authority between the SOURCE and WARPED time
 * domains of a clip (proposal 28 W1). Piecewise-linear through user warp
 * anchors; the scalar stretch factor is the degenerate no-anchor case, and
 * every evaluation is EXACT rational arithmetic (proposal 18 discipline:
 * anchors are integers by nature — born from snapped gestures — and rounding
 * to sample indices happens once, at the render boundary, under floorToInt).
 *
 * Semantics:
 *  - The map passes through the implicit origin (0,0) and every anchor
 *    (src_i, warped_i), strictly increasing in BOTH coordinates.
 *  - Between points: linear interpolation (exact Fractions).
 *  - Beyond the LAST anchor: the final segment's slope continues (with no
 *    anchors: the scalar stretch everywhere — bit-identical to the historic
 *    `pos × stretch` expressions, which the no-anchor gate depends on).
 *  - Before 0: the first segment's slope continues (negative positions only
 *    arise transiently in lead-in math; both directions stay monotone).
 *
 * Validation: anchors that are not strictly increasing in both coordinates
 * are REJECTED at sanitize() (deterministic: keep the longest valid prefix,
 * drop offenders). Construction assumes sanitized input; the deserializer
 * and the future marker UI both sanitize first.
 *
 * Instances are immutable after construction and freely shareable across
 * threads.
 */
struct twWarpAnchor {
    int64_t src    = 0;   // source-domain frames (integer by nature)
    int64_t warped = 0;   // warped-domain frames (integer by nature)

    bool operator==( const twWarpAnchor &o ) const {
        return src == o.src && warped == o.warped;
    }
};

class twWarpMap {
public:
    // No-anchor map: pure scalar stretch.
    explicit twWarpMap( const Fraction &stretch );

    // Anchor map. `anchors` must be sanitized (see sanitize()); `stretch`
    // remains the recorded scalar (serialization compat + the slope used
    // when the anchor list is empty after sanitizing).
    twWarpMap( const std::vector<twWarpAnchor> &anchors,
               const Fraction &stretch );

    // Deterministically drop invalid anchors: sorted by src, then the
    // longest prefix strictly increasing in BOTH coordinates survives
    // (an offender is skipped, scanning continues).
    static std::vector<twWarpAnchor> sanitize( std::vector<twWarpAnchor> in );

    bool hasAnchors() const { return !anchors_.empty(); }
    const std::vector<twWarpAnchor> &anchors() const { return anchors_; }
    const Fraction &stretch() const { return stretch_; }

    // Exact conversions. Both directions are total functions (slope
    // extension beyond the anchor range).
    Fraction srcToWarped( const Fraction &src ) const;
    Fraction warpedToSrc( const Fraction &warped ) const;

    // The render-boundary rule (proposal 18): floor once, here.
    int64_t srcToWarpedFloor( int64_t src ) const {
        return srcToWarped( Fraction( src ) ).floorToInt();
    }
    int64_t warpedToSrcFloor( int64_t warped ) const {
        return warpedToSrc( Fraction( warped ) ).floorToInt();
    }

    bool operator==( const twWarpMap &o ) const {
        return stretch_ == o.stretch_ && anchors_ == o.anchors_;
    }

private:
    // Segment lookup: index of the segment containing src (by source pos).
    size_t segmentForSrc( const Fraction &src ) const;
    size_t segmentForWarped( const Fraction &warped ) const;

    std::vector<twWarpAnchor> anchors_;
    Fraction                  stretch_;
};

#endif
