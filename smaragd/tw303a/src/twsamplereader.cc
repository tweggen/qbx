
#include <string.h>

#include "twsamplereader.h"
#include "twrandomsource.h"

// Defined here, where twSampleReader is a complete type.
twSampleReader *twRandomSource::acquireReader( tw303aEnvironment &env )
{
    twSampleReader *r = new twSampleReader( env, *this );
    r->init();   // allocate plugs/latches, matching every other live component
    return r;
}

twSampleReader::twSampleReader( tw303aEnvironment &env, twRandomSource &src )
    : twComponent( env ),
      src_( src ),
      pos_( 0 )
{
}

twSampleReader::~twSampleReader()
{
}

bool twSampleReader::isSeekable() const
{
    return true;
}

int twSampleReader::seekTo( offset_t newOffset )
{
    pos_ = newOffset;
    return 0;
}

offset_t twSampleReader::tellPos() const
{
    return pos_;
}

length_t twSampleReader::calcOutputTo( sample_t *pDest, length_t length, idx_t idx )
{
    if( length <= 0 ) return 0;
    // The source zero-fills past end-of-material, so the destination is always
    // fully written. Advance by the requested length so a caller that streams
    // without re-seeking stays in lockstep with output time.
    src_.read( pos_, pDest, length, idx );
    pos_ += (offset_t) length;
    return length;
}

void twSampleReader::createOutputLatches()
{
    idx_t n = getNOutputs();
    for( idx_t i = 0; i < n; ++i ) {
        pOutputLatches[i] = new twStreamingLatch( *this, i, 0 );
    }
}

idx_t twSampleReader::getNInputs() const
{
    return 0;
}

idx_t twSampleReader::getNOutputs() const
{
    idx_t ch = src_.channels();
    return ch > 0 ? ch : 1;
}

const char *twSampleReader::getInputName( idx_t ) const
{
    return NULL;
}

const char *twSampleReader::getOutputName( idx_t ) const
{
    return "Sample reader output";
}

// ============================================================================
// Internal State Snapshot Implementation (Phase 1 - Gap 2)
// ============================================================================


std::any twSampleReader::captureInternalState() const
{
    // Capture the current read position for state resumption
    return std::any(InternalState{pos_});
}

void twSampleReader::restoreInternalState(const std::any& state)
{
    try {
        auto s = std::any_cast<const InternalState&>(state);
        pos_ = s.position;
    } catch (const std::bad_any_cast&) {
        // State format mismatch; log warning but don't crash
        fprintf(stderr, "twSampleReader::restoreInternalState: state format mismatch, skipping restore\n");
    }
}
