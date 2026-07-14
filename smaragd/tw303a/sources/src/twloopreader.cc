
#include "tw/sources/twloopreader.h"
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


void twLoopReader::reset()
{
    // Call parent's reset
    twSampleReader::reset();
}
