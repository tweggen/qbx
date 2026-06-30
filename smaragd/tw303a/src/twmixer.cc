
#include <stdio.h>
#include <stdlib.h>
#include "twsyslog.h"
#include <string.h>
#include <math.h>

#include "twmixer.h"
#include "io_vector.h"

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
        if (pInputPlugs[i]) {
            twLatch &latch = pInputPlugs[i]->getParentLatch();
            twComponent &comp = latch.getComponent();
            comp.seekTo(offset);
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
	pOutputLatches[0] = new twStreamingLatch( *this, 0, 0 );
#ifdef DEBUG_COMPONENT
	fprintf( sterr, "twMixer::createOutputLatches(): leaving." );
#endif
}

// Phase 3: IOVector-based interface (type-safe, page-backed rendering)
length_t twMixer::calcOutputTo( IOVector& dest, idx_t idx )
{
    std::lock_guard<std::mutex> lock(mutex());

    // Allocate temp buffer for mixing (reuse strategy: could use a preallocated buffer)
    sample_t *outputBuffer = (sample_t *)alloca(dest.length() * sizeof(sample_t));
    memset(outputBuffer, 0, dest.length() * sizeof(sample_t));

    // Mix all inputs
    length_t realRead = 0;
    InputProperties *props = inputProperties_;

    for( idx_t ch = 0; ch < mixerInputs_; ch++, props++ ) {
        if( !pInputPlugs[ch] ) continue;
        float factor = props->volumeFactor_;

        realRead = ((twLatchStreamingOutput *)pInputPlugs[ch])->readStreamingData( inBuffer, dest.length() );
        if( realRead != dest.length() ) {
            throw new excStandard( "twMixer::calcOutputTo(): Source did not provide sufficient data." );
        }

        sample_t *pCurr = outputBuffer;
        sample_t *pSrc = inBuffer;
        for( offset_t i = 0; i < (offset_t)dest.length(); i++ ) {
            *pCurr++ += *pSrc++ * factor;
        }
    }

    // Write mixed result to IOVector destination
    return dest.copyFrom(IOVector::CreateFromBuffer(outputBuffer, realRead), 0, realRead);
}

/**
 * Change the level of a given input.
 */
int twMixer::setInputLevel( idx_t i, double volume )
{
    if( i>=mixerInputs_ ) {
        fprintf( stderr, "Unable to set input level of %d>%d.\n",
                 i, mixerInputs_ );
        return -1;
    }
    if( !inputProperties_ ) {
        fprintf( stderr, "Unable to set input level of %d, no input properties "
                 "allocated yet.\n",
                 i );
        return -1;
    }
    inputProperties_[i].volume_ = volume;
    inputProperties_[i].volumeFactor_ = 
        (sample_t) pow( 10., volume/20. );
    // FIXME: Set something to enable float/fixed calculation.
    // fprintf( stderr, "Volume set to %f db, volume factor set to $%x.\n",
    //         inputProperties_[i].volume_, 
    //         inputProperties_[i].volumeFactor_ );
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
// calcOutputTo() is running concurrently and dereferencing pInputPlugs array
int twMixer::setNInputs_nolock( idx_t n )
{
    if( n<=0 ) return -2;
    if( n<=mixerInputs_ ) {
        // FIXME: Decrease the actual number of channels
        // connected, but don't fool around
        // in the data structures.
        return 0;
    }
    twLatchOutput **newInputPlugs = (twLatchOutput **) ::calloc (sizeof (twLatchOutput *), n );
    if( !newInputPlugs ) return -1;
    if( mixerInputs_ ) {
        memcpy( newInputPlugs, pInputPlugs, sizeof( twLatchOutput *)*mixerInputs_ );
    }
    twLatchOutput **oldPlugs = pInputPlugs;
    pInputPlugs = newInputPlugs;
    mixerInputs_ = n;
    ::free( oldPlugs );

    // (Re)alloc input properties.
    if( !inputProperties_ ) {
        inputProperties_ = (InputProperties *) ::calloc( n, sizeof( InputProperties ) );
    } else {
        inputProperties_ = (InputProperties *) ::realloc( inputProperties_, n*sizeof( InputProperties ) );
    }
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


