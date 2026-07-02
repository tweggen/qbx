
#include "twloopreader.h"
#include <vector>
#include "twrandomsource.h"
#include <vector>
#include "io_vector.h"
#include <vector>

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

    // Read with loop wrapping (same logic as raw-pointer version)
    offset_t pos = tellPos();
    length_t filled = 0;
    while( filled < dest.length() ) {
        offset_t inLoop = ( pos + (offset_t) filled ) % (offset_t) loopLen_;
        length_t chunk = loopLen_ - (length_t) inLoop;
        if( chunk > dest.length() - filled ) chunk = dest.length() - filled;
        getSource().read( loopBase_ + inLoop, buffer.data() + filled, chunk, idx );
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
