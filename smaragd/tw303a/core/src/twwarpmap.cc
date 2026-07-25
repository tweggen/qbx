#include "tw/core/twwarpmap.h"

#include <algorithm>

// The conversion authority for the source↔warped domains (proposal 28 W1).
// Everything here is exact Fraction arithmetic; see the header for the
// semantic contract. The segment tables below always conceptually include
// the origin (0,0) as point 0 and the anchors as points 1..n.

twWarpMap::twWarpMap( const Fraction &stretch )
    : stretch_( stretch )
{
}

twWarpMap::twWarpMap( const std::vector<twWarpAnchor> &anchors,
                      const Fraction &stretch )
    : anchors_( anchors ),
      stretch_( stretch )
{
}

std::vector<twWarpAnchor> twWarpMap::sanitize( std::vector<twWarpAnchor> in )
{
    std::sort( in.begin(), in.end(),
               []( const twWarpAnchor &a, const twWarpAnchor &b ) {
                   if( a.src != b.src ) return a.src < b.src;
                   return a.warped < b.warped;
               } );
    std::vector<twWarpAnchor> out;
    int64_t lastSrc = 0, lastWarped = 0;   // the implicit origin
    for( const twWarpAnchor &a : in ) {
        if( a.src <= lastSrc || a.warped <= lastWarped )
            continue;   // offender: not strictly increasing in both — drop
        out.push_back( a );
        lastSrc    = a.src;
        lastWarped = a.warped;
    }
    return out;
}

// Point i of the conceptual table: (0,0) for i==0, anchors_[i-1] otherwise.
static inline int64_t ptSrc( const std::vector<twWarpAnchor> &a, size_t i )
{
    return i == 0 ? 0 : a[i - 1].src;
}
static inline int64_t ptWarped( const std::vector<twWarpAnchor> &a, size_t i )
{
    return i == 0 ? 0 : a[i - 1].warped;
}

size_t twWarpMap::segmentForSrc( const Fraction &src ) const
{
    // Segment k spans [pt_k, pt_{k+1}); the last segment extends to +inf.
    // (Negative src falls into segment 0, whose slope extends below 0.)
    const size_t nPts = anchors_.size() + 1;
    size_t k = 0;
    while( k + 1 < nPts && src >= Fraction( ptSrc( anchors_, k + 1 ) ) )
        k++;
    return k;
}

size_t twWarpMap::segmentForWarped( const Fraction &warped ) const
{
    const size_t nPts = anchors_.size() + 1;
    size_t k = 0;
    while( k + 1 < nPts && warped >= Fraction( ptWarped( anchors_, k + 1 ) ) )
        k++;
    return k;
}

Fraction twWarpMap::srcToWarped( const Fraction &src ) const
{
    if( anchors_.empty() )
        return src * stretch_;   // bit-identical to the historic expressions

    const size_t k = segmentForSrc( src );
    const size_t nPts = anchors_.size() + 1;
    // Segment slope: interior segments use their two points; the LAST
    // segment (k == nPts-1) continues the slope of the final anchored
    // segment (or the scalar stretch when only the origin precedes it —
    // impossible here since anchors_ is non-empty: the final anchored
    // segment is (pt_{n-1}, pt_n)).
    size_t a = k, b = k + 1;
    if( b >= nPts ) { a = nPts - 2; b = nPts - 1; }
    const int64_t sA = ptSrc( anchors_, a ),  sB = ptSrc( anchors_, b );
    const int64_t wA = ptWarped( anchors_, a ), wB = ptWarped( anchors_, b );
    // warped = wA + (src − sA) · (wB − wA)/(sB − sA), all exact.
    return Fraction( wA )
         + ( src - Fraction( sA ) ) * Fraction( wB - wA, sB - sA );
}

Fraction twWarpMap::warpedToSrc( const Fraction &warped ) const
{
    if( anchors_.empty() ) {
        // src = warped / stretch — exact; stretch is never zero (the ctor
        // clamp upstream guarantees a positive Fraction).
        return warped / stretch_;
    }

    const size_t k = segmentForWarped( warped );
    const size_t nPts = anchors_.size() + 1;
    size_t a = k, b = k + 1;
    if( b >= nPts ) { a = nPts - 2; b = nPts - 1; }
    const int64_t sA = ptSrc( anchors_, a ),  sB = ptSrc( anchors_, b );
    const int64_t wA = ptWarped( anchors_, a ), wB = ptWarped( anchors_, b );
    return Fraction( sA )
         + ( warped - Fraction( wA ) ) * Fraction( sB - sA, wB - wA );
}
