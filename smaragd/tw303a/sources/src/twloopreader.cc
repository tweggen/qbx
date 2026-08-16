
#include "tw/sources/twloopreader.h"
#include <string.h>
#include <vector>
#include "tw/sources/twrandomsource.h"
#include "tw/core/twtimemap.h"
#include "tw/pages/io_vector.h"

twLoopReader::twLoopReader( tw303aEnvironment &env, twRandomSource &src,
                            offset_t loopBase, length_t loopLen )
    : twSampleReader( env, src ),
      loopBase_( loopBase ),
      loopLen_( loopLen )
{
}

twLoopReader::~twLoopReader()
{
}

// Phase 3: IOVector-based interface (type-safe, page-backed rendering)
length_t twLoopReader::calcOutputTo( IOVector& dest, idx_t idx )
{
    if( dest.length() <= 0 ) return 0;
    if( loopLen_ <= 0 ) {
        // No loop segment: fall back to plain linear read via parent's IOVector method
        return twSampleReader::calcOutputTo( dest, idx );
    }

    // Allocate temp buffer for reading
    std::vector<sample_t> buffer(dest.length());

    // The loop tiling IS a twLoopMap (cut-relative -> loop-window
    // positions): render the block by walking its affine segments
    // (proposal 18 Phase 4 - the wrap arithmetic lives in the map, shared
    // with everything else that reasons about the tiling). All quantities
    // here are integral, so the exact-rational segment endpoints floor
    // without loss.
    offset_t pos = tellPos();
    twLoopMap map( Fraction( (int64_t) loopBase_ ),
                   Fraction( (int64_t) loopLen_ ) );
    length_t filled = 0;
    for( const twMapSegment &seg : map.mapInterval(
             Fraction( (int64_t) pos ),
             Fraction( (int64_t) dest.length() ) ) ) {
        length_t chunk = (length_t) seg.length.floorToInt();
        getSource().read( (offset_t) seg.childStart.floorToInt(),
                          buffer.data() + filled, chunk, idx );
        filled += chunk;
    }
    seekTo( pos + (offset_t) dest.length() );

    // Write to IOVector destination
    return dest.copyFrom(IOVector::CreateFromBuffer(buffer.data(), filled), 0, filled);
}


// The wide counterpart of calcOutputTo() above (proposal 36 §4.3, B3).
//
// The segment walk is computed ONCE and replayed for every channel, so all
// channels see the same tiling from the same cursor and the cursor advances
// exactly once — the seek-once/fill-all/advance-once shape §4.3 requires. Doing
// it the other way round (a channel loop containing a fresh mapInterval from
// tellPos()) would work only because the map is stateless, and would break the
// moment anything in the walk became position-dependent; this ordering makes
// the coherence structural.
length_t twLoopReader::renderPageWide( twOutputPage &page, length_t frames,
                                       const sample_t *input,
                                       length_t inputLength )
{
    if( loopLen_ <= 0 ) {
        // No loop window: identical to a plain reader, including the cursor
        // advance. Delegating keeps the two paths from drifting.
        return twSampleReader::renderPageWide( page, frames, input, inputLength );
    }

    length_t n = frames;
    if( n > (length_t) page.channelFrames() ) n = (length_t) page.channelFrames();
    if( n <= 0 ) return 0;

    const offset_t pos = tellPos();
    twLoopMap map( Fraction( (int64_t) loopBase_ ),
                   Fraction( (int64_t) loopLen_ ) );
    const std::vector<twMapSegment> segs =
        map.mapInterval( Fraction( (int64_t) pos ), Fraction( (int64_t) n ) );

    const idx_t nCh = (idx_t) page.channels();
    for( idx_t c = 0; c < nCh; ++c ) {
        sample_t *dst = page.channelPtr( c );
        length_t filled = 0;
        for( const twMapSegment &seg : segs ) {
            length_t chunk = (length_t) seg.length.floorToInt();
            if( chunk <= 0 ) continue;
            if( filled + chunk > n ) chunk = n - filled;
            if( chunk <= 0 ) break;
            getSource().read( (offset_t) seg.childStart.floorToInt(),
                              dst + filled, chunk, c );
            filled += chunk;
        }
        // A short walk leaves a tail; silence is the honest content for it
        // (the narrow path reports the short length instead, but a page is
        // fixed-capacity and its unwritten tail must not be stale audio).
        if( filled < n ) {
            memset( dst + filled, 0, sizeof( sample_t ) * (size_t)( n - filled ) );
        }
    }

    seekTo( pos + (offset_t) n );
    return n;
}

void twLoopReader::reset()
{
    // Call parent's reset
    twSampleReader::reset();
}
