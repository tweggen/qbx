
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
    // One latch per CHANNEL, matching getNOutputs() (proposal 36 B3). Before
    // this, getNOutputs() said 4 and exactly one latch existed — allocPlugs()
    // sized the vector from the lie, so slots 1..3 were permanently null and a
    // linkOutput(1) would have returned nothing. Nothing in the tree did that,
    // which is why it survived; §4.4 rule (1) now gives each index a meaning.
    idx_t n = getNOutputs();
    for( idx_t i = 0; i < n; ++i ) {
        pOutputLatches_[i] = std::make_shared<twStreamingLatch>( shared_from_this(), i, 0 );
    }
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
    // The decoded source's channel count. Deliberately NOT getSource(), which
    // would resolve viewAtRate() — a resampled view has the SAME channel count
    // (rate conversion is per channel), and getOutputChannels() below is called
    // once per page allocation, inside the component mutex. Asking the sample
    // source for a rate view from there would put a second lock, and possibly a
    // resampler construction, on that path for a number it cannot change.
    // 1 when nothing loaded — a component with zero ports could not be wired.
    idx_t ch = source_ ? source_->channels() : 1;
    return ch > 0 ? ch : 1;
}

idx_t twWavInput::getOutputChannels() const
{
    return getNOutputs();
}

length_t twWavInput::renderPageWide( twOutputPage &page, length_t frames,
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

    if( !source_ ) {
        page.fillSilence();
        return 0;
    }

    // One view, one position, every channel — the §4.3 shape. viewAtRate() is
    // resolved once so two channels of one page can never come from two
    // different cached views.
    twRandomSource *view = source_->viewAtRate( env.getSRate() );
    if( !view ) {
        page.fillSilence();
        return 0;
    }
    const idx_t nCh = (idx_t) page.channels();
    for( idx_t c = 0; c < nCh; ++c ) {
        view->read( playOffset_, page.channelPtr( c ), n, c );
    }
    // NO advance: see the header. This cursor is positioned by its caller.
    return n;
}

twRandomSource *twWavInput::getSource() const
{
    // Hand out the project-rate view, so cut readers and preview play at the
    // correct pitch from a single cached resampled buffer.
    return source_ ? source_->viewAtRate( env.getSRate() ) : NULL;
}

twContentHash twWavInput::contentHash() const
{
    // Always the SOURCE-rate digest (from the decode pass), never a digest of
    // a resampled view — the key must be stable across project rates.
    return source_ ? source_->contentHash() : twContentHash();
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
      source_(),
      loaded_( false ),
      playOffset_( 0 ),
      fileName_( fileName )
{
    if( fileName.isEmpty() ) {
        return;
    }
    source_ = std::make_shared<twSampleSource>( env, fileName_ );
    if( !source_->wasLoaded() ) {
        qWarning( "twWavInput: failed to load \"%s\".\n",
                  (const char *) fileName_.toUtf8().constData() );
        source_.reset();
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
        source_.reset();
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

    std::vector<std::shared_ptr<twComponent> > depsCopy;
    {
        std::lock_guard<std::mutex> lock(mutex());
        depsCopy = dependents_;
    }
    for (auto dep : depsCopy) {
        if (dep) dep->onDependencyTeardown(shared_from_this());
    }

    // WAV input has no children, just mark ZOMBIE
}
