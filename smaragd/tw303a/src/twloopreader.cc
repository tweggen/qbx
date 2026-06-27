
#include "twloopreader.h"
#include "twrandomsource.h"

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

length_t twLoopReader::calcOutputTo( sample_t *pDest, length_t length, idx_t idx )
{
    if( length <= 0 ) return 0;
    if( loopLen_ <= 0 ) {
        // No loop segment: fall back to a plain linear read.
        return twSampleReader::calcOutputTo( pDest, length, idx );
    }

    // tellPos() is the cut-relative position; fill the destination in chunks,
    // each chunk staying inside one pass of the loop segment, wrapping at the
    // segment end. getSource().read() zero-fills past end-of-material, so a loop
    // segment that runs past the content end simply repeats trailing silence.
    offset_t pos = tellPos();
    length_t filled = 0;
    while( filled < length ) {
        offset_t inLoop = ( pos + (offset_t) filled ) % (offset_t) loopLen_;
        length_t chunk = loopLen_ - (length_t) inLoop;
        if( chunk > length - filled ) chunk = length - filled;
        getSource().read( loopBase_ + inLoop, pDest + filled, chunk, idx );
        filled += chunk;
    }
    seekTo( pos + (offset_t) length );
    return length;
}


void twLoopReader::reset()
{
    // Call parent's reset
    twSampleReader::reset();
}
