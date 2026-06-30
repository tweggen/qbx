
#include <stdlib.h>
#include <string.h>

#include "twrewire.h"
#include "io_vector.h"

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
    if (pInputPlugs) {
        for (idx_t i = 0; i < nInputs_; ++i) {
            if (pInputPlugs[i]) {
                // The input plug is a twLatchOutput which may be backed by a twComponent
                // We need to seek the parent latch's component
                twLatch &latch = pInputPlugs[i]->getParentLatch();
                twComponent &comp = latch.getComponent();
                comp.seekTo(offset);
            }
        }
    }
    return 0;
}

void twRewire::init()
{
    twComponent::init();
}

// Phase 3: IOVector-based interface (type-safe, page-backed rendering)
length_t twRewire::calcOutputTo( IOVector& dest, idx_t idx )
{
    std::lock_guard<std::mutex> lock(mutex());

    // If no input wired, fill destination with silence
    if( idx < 0 || idx >= nInputs_ || !pInputPlugs[idx] ) {
        return dest.fillSilence(0, dest.length());
    }

    // Read from input into temp buffer
    sample_t *buffer = (sample_t *)alloca(dest.length() * sizeof(sample_t));
    length_t readFrames = ((twLatchStreamingOutput *)pInputPlugs[idx])->readStreamingData(
        buffer, dest.length());

    // Write to IOVector destination
    return dest.copyFrom(IOVector::CreateFromBuffer(buffer, readFrames), 0, readFrames);
}

int twRewire::setNPlugs( idx_t n )
{
    std::lock_guard<std::mutex> lock(mutex());
    return setNPlugs_nolock(n);
}

// Caller must hold mutex()
// CRITICAL: Must be called under lock because:
// 1. Reallocates pInputPlugs array (use-after-free race with calcOutputTo)
// 2. Reallocates pOutputLatches array (use-after-free race with linkOutput)
// 3. Deletes twStreamingLatch objects (dangling reference race with latch consumers)
int twRewire::setNPlugs_nolock( idx_t n )
{
    if( n < 0 ) return -1;
    if( pInputPlugs && pOutputLatches && n == nInputs_ ) return 0;

    // Refuse to shrink when an outgoing slot is still wired up.
    if( pInputPlugs ) {
        for( int i = n; i < nInputs_; ++i ) {
            if( pInputPlugs[i] ) return -2;
        }
    }

    const idx_t allocN = (n > 0 ? n : 1);
    const int toCopy = (n < nInputs_) ? n : nInputs_;

    twLatchOutput **newPlugs =
        (twLatchOutput **) ::calloc( sizeof( twLatchOutput * ), allocN );
    if( pInputPlugs ) {
        if( toCopy > 0 ) {
            ::memcpy( newPlugs, pInputPlugs, toCopy * sizeof( twLatchOutput * ) );
        }
        ::free( pInputPlugs );
    }
    pInputPlugs = newPlugs;

    twLatch **newLatches =
        (twLatch **) ::calloc( sizeof( twLatch * ), allocN );
    if( pOutputLatches ) {
        if( toCopy > 0 ) {
            ::memcpy( newLatches, pOutputLatches, toCopy * sizeof( twLatch * ) );
        }
        // Latches in slots that are going away need to be deleted.
        for( int i = n; i < nInputs_; ++i ) {
            if( pOutputLatches[i] ) delete pOutputLatches[i];
        }
        ::free( pOutputLatches );
    }
    pOutputLatches = newLatches;

    // Fill any freshly-created slots with their own streaming latch.
    for( int i = 0; i < n; ++i ) {
        if( !pOutputLatches[i] ) {
            pOutputLatches[i] = new twStreamingLatch( *this, i, 0 );
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
// CRITICAL: Lock prevents race with setNPlugs() which may reallocate or delete pOutputLatches
twLatchOutput *twRewire::linkOutput_nolock( idx_t idx )
{
    if( idx < 0 || idx >= nInputs_ ) return NULL;
    if( !pOutputLatches[idx] ) return NULL;
    return pOutputLatches[idx]->addOutput();
}

twRewire::~twRewire()
{
    // Base class frees the pOutputLatches array itself; we have to
    // delete the latch objects it points at first.
    if( pOutputLatches ) {
        for( int i = 0; i < nInputs_; ++i ) {
            if( pOutputLatches[i] ) {
                delete pOutputLatches[i];
                pOutputLatches[i] = NULL;
            }
        }
    }
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

