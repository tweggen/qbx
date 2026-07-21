
#include <stdio.h>
#include <stdlib.h>
#include "tw/core/twsyslog.h"
#include <vector>
#include <string.h>
#include <math.h>

#include "tw/mix/twmixer.h"
#include <vector>
#include "tw/pages/io_vector.h"
#include "tw/core/twlog.h"
#include <vector>

const char *twMixer::getInputName( idx_t ) const
{
    return "Signal input";
}

const char *twMixer::getOutputName( idx_t ) const
{
    return "Signal sum";
}

int twMixer::seekTo( offset_t offset )
{
    std::lock_guard<std::mutex> lock(mutex());
    return seekTo_nolock(offset);
}

// Caller must hold mutex()
int twMixer::seekTo_nolock( offset_t offset )
{
    // Forward the seek to all input plugs
    for (idx_t i = 0; i < mixerInputs_; ++i) {
        if (i < (idx_t)pInputPlugs_.size() && pInputPlugs_[i]) {
            twLatch &latch = pInputPlugs_[i]->getParentLatch();
            std::shared_ptr<twComponent> comp = latch.getComponent();
            comp->seekTo(offset);
        }
    }
    return 0;
}

void twMixer::init()
{
	twComponent::init();
}

void twMixer::createOutputLatches()
{
#ifdef DEBUG_COMPONENT
	fprintf( sterr, "twMixer::createOutputLatches(): entered." );
#endif
	// use default buffer size
#ifdef DEBUG_COMPONENT
	fprintf( sterr, "twMixer::createOutputLatches(): creating streaming latch..." );
#endif
	pOutputLatches_[0] = std::make_shared<twStreamingLatch>( shared_from_this(), 0, 0 );
#ifdef DEBUG_COMPONENT
	fprintf( sterr, "twMixer::createOutputLatches(): leaving." );
#endif
}

// Phase 3: IOVector-based interface (type-safe, page-backed rendering)
// Phase 2: Lock-free snapshot for audio thread
length_t twMixer::calcOutputTo( IOVector& dest, idx_t idx )
{
    // Fast path: Check if component is being torn down
    if (state_.load(std::memory_order_acquire) == ComponentState::ZOMBIE) {
        return dest.fillSilence(0, dest.length());
    }

    // Snapshot input plugs and properties under brief lock, then release before recursive render
    struct InputSnap { std::shared_ptr<twLatchOutput> plug; float factor; };
    std::vector<InputSnap> inputSnapshot;
    {
        std::lock_guard<std::mutex> lock(mutex());
        inputSnapshot.reserve(mixerInputs_);
        for( idx_t ch = 0; ch < mixerInputs_; ++ch ) {
            float volumeFactor = 0.0f;
            if( inputProperties_ ) {
                volumeFactor = inputProperties_[ch].volumeFactor_;
            }
            if( ch < (idx_t)pInputPlugs_.size() && pInputPlugs_[ch] ) {
                inputSnapshot.push_back({
                    pInputPlugs_[ch],  // Copy shared_ptr (refcount++)
                    volumeFactor
                });
            } else {
                inputSnapshot.push_back({ nullptr, volumeFactor });
            }
        }
    }  // Lock released here; inputSnapshot keeps plugs alive

    // Safe: recursive render can proceed; audio thread can try_to_lock immediately
    std::vector<sample_t> outputBuffer(dest.length(), 0.0f);
    std::vector<sample_t> tmpBuffer(dest.length());
    length_t realRead = 0;

    static int processedChannels = 0;
    int validChannels = 0;
    for( const auto& inp : inputSnapshot ) {
        if( !inp.plug ) continue;
        validChannels++;
        realRead = static_cast<twLatchStreamingOutput*>
            (inp.plug.get())->readStreamingData( tmpBuffer.data(), dest.length() );
        if( realRead != dest.length() ) {
            throw new excStandard( "twMixer::calcOutputTo(): Source did not provide sufficient data." );
        }

        sample_t *pCurr = outputBuffer.data();
        sample_t *pSrc = tmpBuffer.data();
        for( offset_t i = 0; i < (offset_t)dest.length(); i++ ) {
            *pCurr++ += *pSrc++ * inp.factor;
        }
    }

    // Write mixed result to IOVector destination
    if (validChannels == 0) {
        static int emptyCount = 0;
        if (++emptyCount % 100 == 1) {
            TW_LOGD( "mix", "twMixer::calcOutputTo() has no valid input channels (snapshot size=%zu)", inputSnapshot.size() );
        }
    }
    return dest.copyFrom(IOVector::CreateFromBuffer(outputBuffer.data(), realRead), 0, realRead);
}

/**
 * Change the level of a given input.
 */
int twMixer::setInputLevel( idx_t i, double volume )
{
    if( i>=mixerInputs_ ) {
        TW_LOGE( "mix", "Unable to set input level of %d>%d.",
                 i, mixerInputs_  );
        return -1;
    }
    if( !inputProperties_ ) {
        TW_LOGE( "mix", "Unable to set input level of %d, no input properties "
                 "allocated yet.",
                 i  );
        return -1;
    }
    inputProperties_[i].volume_ = volume;
    inputProperties_[i].volumeFactor_ = 
        (sample_t) pow( 10., volume/20. );
    // FIXME: Set something to enable float/fixed calculation.
    // TW_LOGD( "mix", "Volume set to %f db, volume factor set to $%x.",
    //         inputProperties_[i].volume_, 
    //         inputProperties_[i].volumeFactor_  );
    return 0;
}

/**
 * Change the number of mixer inputs.
 * The number of channels can only be shrunken, if there is nothing connected.
 * Technically, the number of inputs never becomes smaller.
 *
 * Thread-safe: acquires lock to prevent concurrent reallocation/access races.
 */
int twMixer::setNInputs( idx_t n )
{
    std::lock_guard<std::mutex> lock(mutex());
    return setNInputs_nolock(n);
}

// Caller must hold mutex()
// CRITICAL: Must be called under lock to prevent use-after-free when
// calcOutputTo() is running concurrently and dereferencing pInputPlugs_ array
// Phase 1: Use shared_ptr vectors to prevent dangling references
int twMixer::setNInputs_nolock( idx_t n )
{
    if( n<=0 ) return -2;
    if( n<=mixerInputs_ ) {
        // FIXME: Decrease the actual number of channels
        // connected, but don't fool around
        // in the data structures.
        return 0;
    }

    // Resize vector (shared_ptr destructs old elements automatically)
    pInputPlugs_.resize(n);

    // (Re)alloc input properties.
    if( !inputProperties_ ) {
        inputProperties_ = (InputProperties *) ::calloc( n, sizeof( InputProperties ) );
    } else {
        inputProperties_ = (InputProperties *) ::realloc( inputProperties_, n*sizeof( InputProperties ) );
    }
    mixerInputs_ = n;
    return 0;
}

void twMixer::setBufferSize( length_t len )
{
    std::lock_guard<std::mutex> lock(mutex());
    setBufferSize_nolock(len);
}

// Caller must hold mutex()
// CRITICAL: Lock must be held to prevent use-after-free if
// calcOutputTo_nolock() is reading inBuffer concurrently
void twMixer::setBufferSize_nolock( length_t /* len */ )
{
    if( inBuffer ) free( inBuffer );

    // CRITICAL: Ensure buffer is large enough for page-based rendering.
    // freezePage() requests 65536 samples (256KB page size / sizeof(float)).
    // Even for real-time operation at smaller buffer sizes, we need to accommodate page rendering.
    length_t envSize = env.getBufferSize();
    length_t minSize = 65536;  // Full page capacity
    length_t allocSize = (envSize > minSize) ? envSize : minSize;

    inBuffer = (sample_t *) calloc( sizeof(sample_t), allocSize );
    if( !inBuffer ) {
        throw excStandard( "twMixer::setBufferSize(): Not enough memory for mixer input channels." );
    }
}

twMixer::twMixer( tw303aEnvironment &env0, idx_t inputs )
    : twComponent( env0 ), mixerInputs_(inputs), inputProperties_( NULL )
{
	// alloc frequency buffer
	inBuffer = NULL;
	setBufferSize( env.getBufferSize() );
        // FIXME: This also is written in setNChannels
        inputProperties_ = (InputProperties *) ::calloc( inputs, sizeof( InputProperties ) );
}

void twMixer::reset()
{
	// Stateless mixer: nothing to reset
}

void twMixer::teardown()
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

    // Snapshot inputs before tearing them down
    std::vector<std::shared_ptr<twComponent> > inputsCopy;
    {
        std::lock_guard<std::mutex> lock(mutex());
        for (idx_t i = 0; i < mixerInputs_; ++i) {
            if (i < (idx_t)pInputPlugs_.size() && pInputPlugs_[i]) {
                twLatch &latch = pInputPlugs_[i]->getParentLatch();
                std::shared_ptr<twComponent> comp = latch.getComponent();
                inputsCopy.push_back(comp);
            }
        }
    }

    for (auto &input : inputsCopy) {
        input->teardown();
    }
}


