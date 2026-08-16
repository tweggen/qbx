
#include <string.h>
#include <vector>

#include "tw/sources/twsamplereader.h"
#include "tw/sources/twrandomsource.h"
#include "tw/pages/io_vector.h"
#include "tw/core/twlog.h"

// Defined here, where twSampleReader is a complete type.
std::shared_ptr<twSampleReader> twRandomSource::acquireReader( tw303aEnvironment &env, offset_t initialOffset )
{
    // Must be a shared_ptr before init(): createOutputLatches() calls
    // shared_from_this(), which throws std::bad_weak_ptr on a non-shared object.
    auto r = std::make_shared<twSampleReader>( env, *this );
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
    // Fast path: Check if component is being torn down
    if (state_.load(std::memory_order_acquire) == ComponentState::ZOMBIE) {
        return dest.fillSilence(0, dest.length());
    }

    std::lock_guard<std::mutex> lock(mutex());

    // Allocate temp buffer on heap (not stack) to avoid stack overflow in deep recursion
    std::vector<sample_t> buffer(dest.length());

    // Read from source at current position
    src_.read( pos_, buffer.data(), dest.length(), idx );
    pos_ += (offset_t) dest.length();

    // Write to IOVector destination
    return dest.copyFrom(IOVector::CreateFromBuffer(buffer.data(), dest.length()), 0, dest.length());
}

idx_t twSampleReader::getOutputChannels() const
{
    // Exactly getNOutputs()'s number, for exactly this class's reason: a
    // reader's ports ARE the source's channels. A source that reports 0 (an
    // unloaded file) still owes its consumers a page, and a page must have at
    // least one channel.
    idx_t ch = src_.channels();
    return ch > 0 ? ch : 1;
}

// The wide render path (proposal 36 §4.3). Called by freezePage_nolock exactly
// when the page in hand carries more than one channel; at width 1 the component
// still takes the pre-B3 renderFrames() → calcOutputTo() route, byte for byte.
//
// ONE seek (freezePage_nolock has already done it), ONE pass over the channels,
// ONE cursor advance at the end. src_.read() is stateless and takes the channel
// as an argument, so the whole page is read at the SAME position for every
// channel — which is the property a per-channel loop over calcOutputTo() could
// not have, and the property that makes the two channels of a stereo clip
// coherent rather than one page apart.
length_t twSampleReader::renderPageWide( twOutputPage &page, length_t frames,
                                         const sample_t * /*input*/,
                                         length_t /*inputLength*/ )
{
    if( state_.load( std::memory_order_acquire ) == ComponentState::ZOMBIE ) {
        page.fillSilence();
        return 0;
    }

    length_t n = frames;
    if( n > (length_t) page.channelFrames() ) n = (length_t) page.channelFrames();
    if( n <= 0 ) return 0;

    std::lock_guard<std::mutex> lock( mutex() );

    // The page in hand is the authority on how many channels to fill (§4.4),
    // never getOutputChannels(). src_.read() clamps an out-of-range channel to
    // the last one ("mono plays on every channel"), so a page wider than the
    // source is filled rather than left holding whatever the allocation left.
    const idx_t nCh = (idx_t) page.channels();
    for( idx_t c = 0; c < nCh; ++c ) {
        src_.read( pos_, page.channelPtr( c ), n, c );
    }
    pos_ += (offset_t) n;

    return n;
}

void twSampleReader::createOutputLatches()
{
    idx_t n = getNOutputs();
    for( idx_t i = 0; i < n; ++i ) {
        pOutputLatches_[i] = std::make_shared<twStreamingLatch>( shared_from_this(), i, 0 );
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
        TW_LOGW( "sources", "twSampleReader::restoreInternalState: state format mismatch, skipping restore" );
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

void twSampleReader::teardown()
{
    state_.store(ComponentState::ZOMBIE, std::memory_order_release);

    if (auto parent = parentComponent_.lock()) {
        if (myInputIndex_ >= 0) {
            parent->removeInput(myInputIndex_);
        }
    }

    std::vector<std::shared_ptr<twComponent> > depsCopy;
    {
        std::lock_guard<std::mutex> lock(mutex());
        depsCopy = dependents_;
    }
    for (auto dep : depsCopy) {
        if (dep) dep->onDependencyTeardown(shared_from_this());
    }

    // Sample reader has no children, just mark ZOMBIE
}
