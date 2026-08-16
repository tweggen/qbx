
#include <stdlib.h>
#include <string.h>

#include "tw/mix/twrewire.h"
#include <algorithm>
#include <vector>
#include "tw/pages/io_vector.h"
#include "tw/core/twlog.h"
#include <vector>

const char *twRewire::getInputName( idx_t ) const
{
    return "Rewire input";
}

const char *twRewire::getOutputName( idx_t ) const
{
    return "Rewire output";
}

int twRewire::seekTo( offset_t offset )
{
    std::lock_guard<std::mutex> lock(mutex());
    return seekTo_nolock(offset);
}

// Caller must hold mutex()
int twRewire::seekTo_nolock( offset_t offset )
{
    // Forward the seek to all connected input plugs (the tracks)
    for (idx_t i = 0; i < nInputs_; ++i) {
        if (i < (idx_t)pInputPlugs_.size() && pInputPlugs_[i]) {
            // The input plug is a twLatchOutput which may be backed by a twComponent
            // We need to seek the parent latch's component
            twLatch &latch = pInputPlugs_[i]->getParentLatch();
            std::shared_ptr<twComponent> comp = latch.getComponent();
            comp->seekTo(offset);
        }
    }
    return 0;
}

void twRewire::init()
{
    twComponent::init();
}

// Phase 3: IOVector-based interface (type-safe, page-backed rendering)
// Phase 2: Lock-free snapshot for audio thread
length_t twRewire::calcOutputTo( IOVector& dest, idx_t idx )
{
    // Fast path: Check if component is being torn down
    if (state_.load(std::memory_order_acquire) == ComponentState::ZOMBIE) {
        return dest.fillSilence(0, dest.length());
    }

    // Snapshot input plug under brief lock, then release before recursive render
    std::shared_ptr<twLatchOutput> inputPlugSnapshot;
    {
        std::lock_guard<std::mutex> lock(mutex());
        if( idx < 0 || idx >= nInputs_ || idx >= (idx_t)pInputPlugs_.size() || !pInputPlugs_[idx] ) {
            static int silenceCount = 0;
            if (++silenceCount % 100 == 1) {
                TW_LOGD( "mix", "twRewire::calcOutputTo() returning silence: idx=%d, nInputs=%d, pPlugs.size=%zu, plug=%s",
                    (int)idx, (int)nInputs_, pInputPlugs_.size(),
                    (idx >= (idx_t)pInputPlugs_.size()) ? "OOB" : (!pInputPlugs_[idx] ? "null" : "valid") );
            }
            return dest.fillSilence(0, dest.length());
        }
        inputPlugSnapshot = pInputPlugs_[idx];  // Copy shared_ptr (refcount++)
    }  // Lock released here; inputPlugSnapshot keeps plug alive

    // Safe: recursive render can proceed; audio thread can try_to_lock immediately
    // Read from input into temp buffer (heap-allocated to avoid stack overflow in deep recursion)
    std::vector<sample_t> buffer(dest.length());
    length_t readFrames = static_cast<twLatchStreamingOutput*>
        (inputPlugSnapshot.get())->readStreamingData(buffer.data(), dest.length());

    // Write to IOVector destination
    return dest.copyFrom(IOVector::CreateFromBuffer(buffer.data(), readFrames), 0, readFrames);
}

// --- Proposal 36 B4: width and channel mapping ------------------------------

void twRewire::setChannels( idx_t n )
{
    if( n < 1 ) n = 1;
    if( channels_.exchange( (int) n, std::memory_order_acq_rel ) == (int) n ) {
        return;
    }
    // Cached pages of the old width read as misses (§4.5); bump so they are
    // re-frozen rather than left to be rejected for the rest of the session.
    bumpContentEpoch();
}

void twRewire::setChannelMap( const std::vector<idx_t> &map )
{
    {
        std::lock_guard<std::mutex> lock( mapMutex_ );
        if( channelMap_ == map ) return;
        channelMap_ = map;
    }
    bumpContentEpoch();
}

std::vector<idx_t> twRewire::channelMap() const
{
    std::lock_guard<std::mutex> lock( mapMutex_ );
    return channelMap_;
}

// The wide render (§4.3). One input PAGE, one pass, every output channel filled
// from the mapped input channel. Nothing is copied per channel that could be
// aliased: `page` is ours and `src` is the producer's, always distinct pages.
length_t twRewire::renderPageWide( twOutputPage &page, length_t frames,
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

    const idx_t nCh = (idx_t) page.channels();

    std::vector<idx_t> map = channelMap();
    std::shared_ptr<twOutputPage> src = fetchInputPage( 0, page.startPosition );

    if( !src || src->validAspects == 0 ) {
        for( idx_t c = 0; c < nCh; ++c ) {
            sample_t *dst = page.channelPtr( c );
            std::fill( dst, dst + n, 0.0f );
        }
        return n;
    }

    const length_t m = std::min<length_t>( n, (length_t) src->validFrames );

    for( idx_t c = 0; c < nCh; ++c ) {
        sample_t *dst = page.channelPtr( c );
        const idx_t want = ( c < (idx_t) map.size() ) ? map[c] : c;
        const sample_t *s = src->channelPtr( twPageClampChannel( *src, want ) );
        if( m > 0 ) std::copy( s, s + m, dst );
        if( m < n ) std::fill( dst + m, dst + n, 0.0f );
    }

    return n;
}

int twRewire::setNPlugs( idx_t n )
{
    std::lock_guard<std::mutex> lock(mutex());
    return setNPlugs_nolock(n);
}

// Caller must hold mutex()
// CRITICAL: Must be called under lock because:
// 1. Reallocates pInputPlugs_ vector (use-after-free race with calcOutputTo)
// 2. Reallocates pOutputLatches_ vector (use-after-free race with linkOutput)
// Phase 1: Use shared_ptr to prevent dangling references; RAII handles cleanup
int twRewire::setNPlugs_nolock( idx_t n )
{
    if( n < 0 ) return -1;

    // If size unchanged AND vectors are already initialized, no-op
    if( n == nInputs_ && (size_t)n == pInputPlugs_.size() ) return 0;

    // Refuse to shrink when an outgoing slot is still wired up.
    for( int i = n; i < nInputs_; ++i ) {
        if( i < (int)pInputPlugs_.size() && pInputPlugs_[i] ) {
            return -2;
        }
    }

    // Resize vectors (shared_ptr destructs old elements automatically on shrink)
    pInputPlugs_.resize(n);
    pOutputLatches_.resize(n);

    // Fill any freshly-created slots with their own streaming latch.
    for( int i = 0; i < n; ++i ) {
        if( !pOutputLatches_[i] ) {
            pOutputLatches_[i] = std::make_shared<twStreamingLatch>( shared_from_this(), i, 0 );
        }
    }

    nInputs_ = n;
    return 0;
}

idx_t twRewire::getNInputs() const
{
    return nInputs_;
}

idx_t twRewire::getNOutputs() const
{
    return nInputs_;
}

/**
 * Overridden: pInputPlugs and pOutputLatches are sized/managed by
 * setNPlugs(), which also creates the per-output latches.
 */
void twRewire::allocPlugs()
{
    setNPlugs( nInputs_ );
}

/**
 * Overridden: setNPlugs() already created the streaming latches; nothing
 * to do here.
 */
void twRewire::createOutputLatches()
{
}

twLatchOutput *twRewire::linkOutput( idx_t idx )
{
    std::lock_guard<std::mutex> lock(mutex());
    return linkOutput_nolock(idx);
}

// Caller must hold mutex()
// CRITICAL: Lock prevents race with setNPlugs() which may reallocate pOutputLatches_
twLatchOutput *twRewire::linkOutput_nolock( idx_t idx )
{
    if( idx < 0 || idx >= nInputs_ ) return nullptr;
    if( idx >= (idx_t)pOutputLatches_.size() || !pOutputLatches_[idx] ) return nullptr;
    return pOutputLatches_[idx]->addOutput();
}

twRewire::~twRewire()
{
    // Phase 1: Vectors and shared_ptr handle RAII cleanup automatically
    // Latches in pOutputLatches_ are destroyed when removed from vector
}

twRewire::twRewire( tw303aEnvironment &env0 )
    : twComponent( env0 )
{
    setBufferSize( env.getBufferSize() );
    // Default: 2 channels (stereo L/R pair)
    nInputs_ = 2;
}

void twRewire::reset()
{
	// Stateless router: nothing to reset
}

void twRewire::teardown()
{
    state_.store(ComponentState::ZOMBIE, std::memory_order_release);

    // Deregister from parent mixer
    if (auto parent = parentComponent_.lock()) {
        if (myInputIndex_ >= 0) {
            parent->removeInput(myInputIndex_);
        }
    }

    // Notify dependents
    std::vector<std::shared_ptr<twComponent> > depsCopy;
    {
        std::lock_guard<std::mutex> lock(mutex());
        depsCopy = dependents_;
    }
    for (auto dep : depsCopy) {
        if (dep) dep->onDependencyTeardown(shared_from_this());
    }

    // Snapshot and tear down all track inputs
    std::vector<std::shared_ptr<twComponent>> inputsCopy;
    {
        std::lock_guard<std::mutex> lock(mutex());
        for (idx_t i = 0; i < nInputs_; ++i) {
            if (i < (idx_t)pInputPlugs_.size() && pInputPlugs_[i]) {
                twLatch &latch = pInputPlugs_[i]->getParentLatch();
                std::shared_ptr<twComponent> comp = latch.getComponent();
                // Use shared_ptr with no-op deleter to keep alive during recursion
                inputsCopy.push_back(comp);
            }
        }
    }

    // Recursive teardown (lock released above)
    for (auto &input : inputsCopy) {
        input->teardown();
    }
}

