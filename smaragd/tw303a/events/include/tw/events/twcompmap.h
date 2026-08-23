#ifndef _TW_COMPMAP_H_
#define _TW_COMPMAP_H_

#include <cstdint>
#include <vector>

/**
 * twCompMap — WHICH TAKE OF A COLUMN SOUNDS WHERE (proposal 43 N1).
 *
 * An immutable, sorted list of segments in the COLUMN's own frame domain — the
 * same domain the takes' windows are addressed in, which is NOT the placement's
 * domain when a wrapper still windows the column (proposal 42 M4 refuses to
 * migrate a looping / stretched / panned / automated wrapper, so those survive).
 *
 * Modelled on `twAutomationCurve`, deliberately and in the same slice: the UI
 * edits the model, the model builds a NEW map, the consuming component swaps
 * the pointer, and the component READS IT BY POSITION at freeze time. Nothing
 * here allocates or locks while a page is rendered.
 *
 * THE EMPTY MAP IS THE DEGENERATE CASE AND IT IS LOAD-BEARING. An empty map
 * means "the column's `activeTake_` everywhere". That is what makes every
 * project written before this proposal — and every golden, and the whole of
 * proposal 42's gate set — correct and unchanged: no map, no behaviour change.
 * `takeAt()` reports -1 for it, and the caller substitutes the active index.
 *
 * A segment's take governs [seg.at, next.at), the same left-point convention
 * `twAutomationCurve` uses and every DAW's curve editor draws. `xfade` is the
 * length of the crossfade CENTRED on the segment's own left boundary, so it
 * belongs to the BOUNDARY rather than to either take; the first segment's
 * `xfade` is meaningless and is kept at 0 by `normalized()`.
 */

struct twCompSegment {
    int64_t at    = 0;    // column frame this segment starts at
    int     take  = 0;    // take index it selects
    int64_t xfade = 0;    // crossfade length CENTRED on `at`; 0 = a hard cut

    bool operator==( const twCompSegment &o ) const {
        return at == o.at && take == o.take && xfade == o.xfade;
    }
};

class twCompMap
{
public:
    twCompMap() = default;
    explicit twCompMap( std::vector<twCompSegment> segs )
        : segs_( std::move( segs ) ) {}

    bool empty() const { return segs_.empty(); }
    size_t size() const { return segs_.size(); }
    const std::vector<twCompSegment> &segments() const { return segs_; }
    const twCompSegment &at( size_t i ) const { return segs_[ i ]; }

    /**
     * The take sounding at `pos`, or -1 when this map does not say — an empty
     * map, or a position before the first segment. The caller substitutes the
     * column's `activeTake_`, which is what the empty map MEANS.
     */
    int takeAt( int64_t pos ) const
    {
        int found = -1;
        for( const twCompSegment &s : segs_ ) {
            if( s.at > pos ) break;
            found = s.take;
        }
        return found;
    }

    /**
     * The segment index covering `pos`, or -1. Separate from `takeAt` because
     * a crossfade needs the BOUNDARY, not just the take.
     */
    int segmentAt( int64_t pos ) const
    {
        int found = -1;
        for( size_t i = 0; i < segs_.size(); ++i ) {
            if( segs_[ i ].at > pos ) break;
            found = (int) i;
        }
        return found;
    }

    /**
     * SORTED, DE-DUPLICATED, and anchored: segments ascending by position, a
     * later write at an existing position REPLACING the earlier one (the
     * commonest comping gesture there is — clicking a boundary you already
     * made), the first segment's crossfade cleared, and negative positions
     * dropped. Every mutating verb builds its result through here so the
     * invariant cannot be violated by a caller.
     *
     * A stable fold, not `std::unique`: `std::unique` keeps the FIRST of an
     * equal run, which is precisely the bug proposal 37 P6 found in
     * `SAutomationLane::setPoints` — an edit landing on an existing position
     * was silently DROPPED while the docs promised a replace.
     */
    twCompMap normalized() const
    {
        std::vector<twCompSegment> v;
        v.reserve( segs_.size() );
        for( const twCompSegment &s : segs_ ) {
            if( s.at < 0 ) continue;
            size_t j = v.size();
            while( j > 0 && v[ j - 1 ].at > s.at ) --j;
            if( j > 0 && v[ j - 1 ].at == s.at ) v[ j - 1 ] = s;   // REPLACE
            else v.insert( v.begin() + (long) j, s );
        }
        if( !v.empty() ) v.front().xfade = 0;   // no boundary to fade across
        return twCompMap( std::move( v ) );
    }

    bool operator==( const twCompMap &o ) const { return segs_ == o.segs_; }
    bool operator!=( const twCompMap &o ) const { return !( *this == o ); }

private:
    std::vector<twCompSegment> segs_;
};

#endif // _TW_COMPMAP_H_
