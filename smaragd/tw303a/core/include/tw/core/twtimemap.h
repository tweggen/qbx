#ifndef _TWTIMEMAP_H_
#define _TWTIMEMAP_H_

#include <vector>
#include "tw/core/twfraction.h"

/**
 * First-class, bidirectional, interval-aware time maps (proposal 18
 * Phase 4). A twTimeMap translates positions between a PARENT domain and a
 * CHILD domain with EXACT rational endpoints; rounding to sample indices is
 * the caller's explicit, render-boundary decision (Fraction::floorToInt,
 * starts floored, exclusive ends as floor(start+len)).
 *
 * The maps in the clip chain (POSITION_DOMAINS.md):
 *   - twAffineMap:  child = parent * scale + offset. Covers the link shift
 *                   (scale 1), the cut window shift (scale 1), and the
 *                   stretch factor (scale = stretch). Affine maps COMPOSE
 *                   exactly into one affine map.
 *   - twLoopMap:    child = base + (parent mod len) - the loop tiling.
 *                   Piecewise-affine; mapInterval() returns the wrap
 *                   segments (this is twLoopReader's chunk loop, extracted).
 *
 * Preview and playback consume the SAME map objects (via
 * SCut::makeTimeMap), so the "display right, audio wrong" divergence class
 * is structurally impossible: there is no second implementation to
 * disagree.
 */

// One maximal affine piece of a mapped interval. Positions are exact.
struct twMapSegment {
    Fraction parentStart;   // where this piece begins in the parent domain
    Fraction childStart;    // its image in the child domain
    Fraction length;        // piece length (parent domain; scale maps it)
};

class twTimeMap {
public:
    virtual ~twTimeMap() {}

    // Exact point mappings. inverse() returns the CANONICAL preimage for
    // non-injective maps (the loop's first iteration).
    virtual Fraction map( const Fraction &parentPos ) const = 0;
    virtual Fraction inverse( const Fraction &childPos ) const = 0;

    // Split [start, start+len) into maximal affine pieces, in parent order.
    // The pieces tile the input: piece[k+1].parentStart ==
    // piece[k].parentStart + piece[k].length, no gaps, no overlaps.
    virtual std::vector<twMapSegment> mapInterval( const Fraction &start,
                                                   const Fraction &len ) const = 0;

    virtual bool isAffine() const = 0;
};

// child = parent * scale + offset (exact).
class twAffineMap : public twTimeMap {
public:
    twAffineMap( const Fraction &scale, const Fraction &offset )
        : scale_( scale ), offset_( offset ) {}

    Fraction map( const Fraction &p ) const override {
        return p * scale_ + offset_;
    }
    Fraction inverse( const Fraction &c ) const override {
        return ( c - offset_ ) / scale_;
    }
    std::vector<twMapSegment> mapInterval( const Fraction &start,
                                           const Fraction &len ) const override {
        return { { start, map( start ), len } };
    }
    bool isAffine() const override { return true; }

    // Exact composition: (this ∘ inner)(p) = map(inner.map(p)).
    twAffineMap composedWith( const twAffineMap &inner ) const {
        return twAffineMap( scale_ * inner.scale_,
                            inner.offset_ * scale_ + offset_ );
    }

    const Fraction &scale() const { return scale_; }
    const Fraction &offset() const { return offset_; }

private:
    Fraction scale_;
    Fraction offset_;
};

// child = base + (parent mod len): the loop tiling (piecewise-affine,
// non-injective). parent positions < 0 are clamped to 0.
class twLoopMap : public twTimeMap {
public:
    twLoopMap( const Fraction &base, const Fraction &len )
        : base_( base ), len_( len > Fraction(0) ? len : Fraction(1) ) {}

    Fraction map( const Fraction &p ) const override {
        return base_ + positionInLoop( p );
    }
    // Canonical preimage: the FIRST iteration.
    Fraction inverse( const Fraction &c ) const override {
        return c - base_;
    }
    std::vector<twMapSegment> mapInterval( const Fraction &start,
                                           const Fraction &len ) const override {
        std::vector<twMapSegment> out;
        Fraction pos = start;
        Fraction remaining = len;
        while( remaining > Fraction(0) ) {
            Fraction inLoop = positionInLoop( pos );
            Fraction chunk = len_ - inLoop;          // to the wrap point
            if( chunk > remaining ) chunk = remaining;
            out.push_back( { pos, base_ + inLoop, chunk } );
            pos = pos + chunk;
            remaining = remaining - chunk;
        }
        return out;
    }
    bool isAffine() const override { return false; }

    // ALL preimages of child interval [cStart, cStart+cLen) within the
    // parent range [rangeStart, rangeStart+rangeLen) - what exact
    // invalidation needs (an edit to a source region dirties every timeline
    // image of it under the tiling).
    std::vector<twMapSegment> preimagesWithin( const Fraction &cStart,
                                               const Fraction &cLen,
                                               const Fraction &rangeStart,
                                               const Fraction &rangeLen ) const {
        std::vector<twMapSegment> out;
        // Clamp the queried child window to the loop segment [base, base+len)
        Fraction inStart = cStart - base_;
        Fraction inEnd = inStart + cLen;
        if( inStart < Fraction(0) ) inStart = Fraction(0);
        if( inEnd > len_ ) inEnd = len_;
        if( !( inStart < inEnd ) ) return out;
        // First tile whose image can reach the range start
        Fraction rangeEnd = rangeStart + rangeLen;
        int64_t k0 = ( ( rangeStart - inEnd ) / len_ ).floorToInt() + 1;
        if( k0 < 0 ) k0 = 0;
        for( int64_t k = k0; ; ++k ) {
            Fraction pStart = Fraction( k ) * len_ + inStart;
            if( !( pStart < rangeEnd ) ) break;
            Fraction pEnd = Fraction( k ) * len_ + inEnd;
            Fraction s = pStart < rangeStart ? rangeStart : pStart;
            Fraction e = pEnd > rangeEnd ? rangeEnd : pEnd;
            if( s < e )
                out.push_back( { s, base_ + positionInLoop( s ), e - s } );
        }
        return out;
    }

    const Fraction &base() const { return base_; }
    const Fraction &length() const { return len_; }

private:
    Fraction positionInLoop( const Fraction &p ) const {
        if( p < Fraction(0) ) return Fraction(0);
        int64_t wraps = ( p / len_ ).floorToInt();
        return p - Fraction( wraps ) * len_;
    }

    Fraction base_;
    Fraction len_;
};

#endif // _TWTIMEMAP_H_
