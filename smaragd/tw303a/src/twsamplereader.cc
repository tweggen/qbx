
#include <string.h>

#include "twsamplereader.h"
#include "twrandomsource.h"
#include "io_vector.h"

// Defined here, where twSampleReader is a complete type.
twSampleReader *twRandomSource::acquireReader( tw303aEnvironment &env, offset_t initialOffset )
{
    twSampleReader *r = new twSampleReader( env, *this );
    r->init();   // allocate plugs/latches, matching every other live component
    if( initialOffset != 0 ) {
        r->seekTo( initialOffset );  // Position reader at initial offset
    }
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
    std::lock_guard<std::mutex> lock(mutex());
    return seekTo_nolock(newOffset);
}

// Caller must hold mutex()
int twSampleReader::seekTo_nolock(offset_t newOffset)
{
    pos_ = newOffset;
    return 0;
}

offset_t twSampleReader::tellPos() const
{
    return pos_;
}

// Phase 3: IOVector-based interface (type-safe, page-backed rendering)
length_t twSampleReader::calcOutputTo( IOVector& dest, idx_t idx )
{
    std::lock_guard<std::mutex> lock(mutex());

    // Allocate temp buffer for reading from source
    sample_t *buffer = (sample_t *)alloca(dest.length() * sizeof(sample_t));

    // Read from source at current position
    src_.read( pos_, buffer, dest.length(), idx );
    pos_ += (offset_t) dest.length();

    // Write to IOVector destination
    return dest.copyFrom(IOVector::CreateFromBuffer(buffer, dest.length()), 0, dest.length());
}

// Legacy: Raw-pointer interface
length_t twSampleReader::calcOutputTo( sample_t *pDest, length_t length, idx_t idx )
{
    std::lock_guard<std::mutex> lock(mutex());
    return calcOutputTo_nolock(pDest, length, idx);
}

// Caller must hold mutex()
length_t twSampleReader::calcOutputTo_nolock( sample_t *pDest, length_t length, idx_t idx )
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
    std::lock_guard<std::mutex> lock(mutex());
    // Capture the current read position for state resumption
    return std::any(InternalState{pos_});
}

void twSampleReader::restoreInternalState(const std::any& state)
{
    std::lock_guard<std::mutex> lock(mutex());
    try {
        auto s = std::any_cast<const InternalState&>(state);
        pos_ = s.position;
    } catch (const std::bad_any_cast&) {
        // State format mismatch; log warning but don't crash
        fprintf(stderr, "twSampleReader::restoreInternalState: state format mismatch, skipping restore\n");
    }
}

// ============================================================================
// Phase 1 - Gap 7: Reset Method Implementation
// ============================================================================

void twSampleReader::reset()
{
    std::lock_guard<std::mutex> lock(mutex());
    reset_nolock();
}

// Caller must hold mutex()
void twSampleReader::reset_nolock()
{
    // Reset to beginning of sample
    pos_ = 0;
}
