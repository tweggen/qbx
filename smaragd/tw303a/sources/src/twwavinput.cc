
#include <stdlib.h>
#include <string.h>

#include <qstring.h>

#include "tw/sources/twwavinput.h"
#include <vector>
#include "tw/sources/twsamplesource.h"
#include <vector>
#include "tw/pages/io_vector.h"
#include <vector>

void twWavInput::createOutputLatches()
{
    pOutputLatches_[0] = std::make_shared<twStreamingLatch>( *this, 0, 0 );
}

int twWavInput::setNOutputs( idx_t )
{
    return -1;
}

length_t twWavInput::getLength() const
{
    // Report duration in PROJECT-rate frames, so the timeline reserves the right
    // span for an off-rate sample (otherwise it would be truncated/padded).
    return source_ ? source_->viewAtRate( env.getSRate() )->length() : -1;
}

// Cache sizing is obsolete now that the whole file is resident; kept as no-op
// stubs so the public API is unchanged.
length_t twWavInput::setCacheSize( length_t )
{
    return 0;
}

length_t twWavInput::getCacheSize() const
{
    return 0;
}

bool twWavInput::isSeekable() const
{
    return true;
}

int twWavInput::seekTo( offset_t newOffset )
{
    std::lock_guard<std::mutex> lock(mutex());
    return seekTo_nolock(newOffset);
}

// Caller must hold mutex()
int twWavInput::seekTo_nolock(offset_t newOffset)
{
    playOffset_ = newOffset;
    return 0;
}

const char *twWavInput::getInputName( idx_t ) const
{
    return NULL;
}

const char *twWavInput::getOutputName( idx_t ) const
{
    return (const char *) fileName_.data();
}

idx_t twWavInput::getNInputs() const
{
    return 0;
}

idx_t twWavInput::getNOutputs() const
{
    return 4;
}

twRandomSource *twWavInput::getSource() const
{
    // Hand out the project-rate view, so cut readers and preview play at the
    // correct pitch from a single cached resampled buffer.
    return source_ ? source_->viewAtRate( env.getSRate() ) : NULL;
}

/**
 * Serve audio by random-reading the resident source at the current play
 * position. This is the shared/back-compat cursor; it does not auto-advance,
 * matching the historical contract where callers seek before every block.
 */

// Phase 3: IOVector-based interface (type-safe, page-backed rendering)
length_t twWavInput::calcOutputTo( IOVector& dest, idx_t idx )
{
    // Fast path: Check if component is being torn down
    if (state_.load(std::memory_order_acquire) == ComponentState::ZOMBIE) {
        return dest.fillSilence(0, dest.length());
    }

    std::lock_guard<std::mutex> lock(mutex());

    if( !source_ ) {
        return dest.fillSilence(0, dest.length());
    }

    // Read through the project-rate view into temp buffer
    std::vector<sample_t> buffer(dest.length());
    source_->viewAtRate( env.getSRate() )->read( playOffset_, buffer.data(), dest.length(), idx );

    // Write to IOVector destination
    return dest.copyFrom(IOVector::CreateFromBuffer(buffer.data(), dest.length()), 0, dest.length());
}

void twWavInput::init()
{
    twComponent::init();
}

void twWavInput::setBufferSize( length_t )
{
}

twWavInput::twWavInput( tw303aEnvironment &env, QString fileName )
    : twComponent( env ),
      source_( NULL ),
      loaded_( false ),
      playOffset_( 0 ),
      fileName_( fileName )
{
    if( fileName.isEmpty() ) {
        return;
    }
    source_ = new twSampleSource( env, fileName_ );
    if( !source_->wasLoaded() ) {
        qWarning( "twWavInput: failed to load \"%s\".\n",
                  (const char *) fileName_.toUtf8().constData() );
        delete source_;
        source_ = NULL;
        return;
    }
    loaded_ = true;
    // Build the resampled view now, on the (UI) load thread, so the one-time
    // resample cost does not land in the first realtime audio block. No-op when
    // the sample is already at the project rate.
    source_->viewAtRate( env.getSRate() );
}

twWavInput::~twWavInput()
{
    if( source_ ) {
        delete source_;
        source_ = NULL;
    }
}


void twWavInput::reset()
{
    std::lock_guard<std::mutex> lock(mutex());
    reset_nolock();
}

// Caller must hold mutex()
void twWavInput::reset_nolock()
{
    // Reset playback position to start
    playOffset_ = 0;
}

void twWavInput::teardown()
{
    state_.store(ComponentState::ZOMBIE, std::memory_order_release);

    if (auto parent = parentComponent_.lock()) {
        if (myInputIndex_ >= 0) {
            parent->removeInput(myInputIndex_);
        }
    }

    std::vector<twComponent*> depsCopy;
    {
        std::lock_guard<std::mutex> lock(mutex());
        depsCopy = dependents_;
    }
    for (auto dep : depsCopy) {
        if (dep) dep->onDependencyTeardown(this);
    }

    // WAV input has no children, just mark ZOMBIE
}
