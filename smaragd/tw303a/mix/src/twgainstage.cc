#include "tw/mix/twgainstage.h"

#include "tw/graph/tw303aenv.h"
#include "tw/pages/io_vector.h"
#include "tw/core/twlog.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// ------------------------------------------------------------------ lifecycle

twGainStage::twGainStage( tw303aEnvironment &env0 )
    : twComponent( env0 )
{
    setBufferSize( env.getBufferSize() );
}

twGainStage::~twGainStage() = default;

const char *twGainStage::getInputName( idx_t ) const
{
    return "Gain stage input";
}

const char *twGainStage::getOutputName( idx_t ) const
{
    return "Gain stage output";
}

void twGainStage::createOutputLatches()
{
    // ONE port. The channel dimension is in the page (proposal 36 4.2).
    pOutputLatches_.resize( 1 );
    pOutputLatches_[0] = std::make_shared<twStreamingLatch>( shared_from_this(), 0, 4096 );
}

int twGainStage::seekTo( offset_t offset )
{
    // freezePage_nolock() calls this immediately before the render, inside
    // cursorMutex_, which is how the narrow render learns the position its
    // signature does not carry. Deliberately NOT forwarded upstream: this
    // component reads its producer POSITIONALLY (fetchInputPage(0, pageStart)),
    // so moving the producer's cursor from here would only race its own freeze --
    // the same rule twPluginInsert::seekTo and twPluginChain::seekTo follow.
    renderPos_.store( offset, std::memory_order_relaxed );
    return 0;
}

void twGainStage::reset()
{
    // Class infinity, pure: there is no DSP state to clear. The output of a
    // frame is a function of that frame's input and its POSITION, which is why
    // range invalidation over this component is exact.
}

// --------------------------------------------------------------------- params

void twGainStage::setGainDb( double gainDb )
{
    {
        std::lock_guard<std::mutex> lock( paramMutex_ );
        if( gainDb_ == gainDb ) return;
        gainDb_ = gainDb;
    }
    // The scalar is baked into every page we have already published.
    bumpContentEpoch();
}

double twGainStage::gainDb() const
{
    std::lock_guard<std::mutex> lock( paramMutex_ );
    return gainDb_;
}

void twGainStage::setMuted( bool muted, offset_t atFrame )
{
    {
        std::lock_guard<std::mutex> lock( paramMutex_ );
        if( muted_ == muted && muteAnchor_ == atFrame ) return;
        muted_      = muted;
        muteAnchor_ = atFrame;
    }
    bumpContentEpoch();
}

bool twGainStage::muted() const
{
    std::lock_guard<std::mutex> lock( paramMutex_ );
    return muted_;
}

length_t twGainStage::muteRampFrames() const
{
    // ~1.5 ms, the middle of the 1-2 ms the design asks for. Rate-derived, so a
    // 96 kHz project gets the same TIME rather than the same frame count.
    const int sr = env.getSRate();
    const length_t n = (length_t) ( ( sr > 0 ? sr : 48000 ) * 3 / 2000 );
    return n > 0 ? n : 1;
}

void twGainStage::setChannels( idx_t n )
{
    if( n < 1 ) n = 1;
    if( channels_.exchange( (int) n, std::memory_order_acq_rel ) == (int) n ) {
        return;
    }
    // Cached pages of the old width read as misses (proposal 36 4.5); bump so
    // they are re-frozen rather than rejected for the rest of the session.
    bumpContentEpoch();
}

// ------------------------------------------------------------------- envelope

twGainStage::Envelope twGainStage::envelope() const
{
    Envelope e;
    {
        std::lock_guard<std::mutex> lock( paramMutex_ );
        e.base       = std::pow( 10., gainDb_ / 20. );
        e.muted      = muted_;
        e.muteAnchor = muteAnchor_;
    }
    e.ramp = muteRampFrames();
    return e;
}

double twGainStage::factorAt( const Envelope &e, offset_t pos )
{
    if( e.muteAnchor == kRampImmediate ) {
        return e.muted ? 0.0 : e.base;
    }
    const offset_t d = pos - e.muteAnchor;
    double m;
    if( d <= 0 ) {
        m = e.muted ? 1.0 : 0.0;                        // before the change
    } else if( d >= (offset_t) e.ramp ) {
        m = e.muted ? 0.0 : 1.0;                        // after it
    } else {
        const double t = (double) d / (double) e.ramp;
        m = e.muted ? ( 1.0 - t ) : t;
    }
    return e.base * m;
}

bool twGainStage::isFlat( const Envelope &e, offset_t start, length_t n )
{
    if( e.muteAnchor == kRampImmediate ) return true;
    if( n <= 0 ) return true;
    // Flat iff the whole span sits entirely on one side of the ramp.
    const offset_t end = start + n;                              // exclusive
    if( end <= e.muteAnchor ) return true;                       // wholly before
    if( start >= e.muteAnchor + (offset_t) e.ramp ) return true; // wholly after
    return false;
}

void twGainStage::applyGain( const sample_t *src, sample_t *dst, length_t n,
                             offset_t start, const Envelope &e )
{
    if( n <= 0 ) return;

    if( isFlat( e, start, n ) ) {
        const double f = factorAt( e, start );
        // UNITY IS A PURE COPY, with no multiply at all. This is what makes the
        // fader move byte-identical over a corpus that never touches a fader:
        // the samples that come out are the ones that went in.
        if( f == 1.0 ) {
            if( dst != src ) std::copy( src, src + n, dst );
            return;
        }
        if( f == 0.0 ) {
            std::fill( dst, dst + n, 0.0f );
            return;
        }
        const sample_t g = (sample_t) f;
        for( length_t i = 0; i < n; ++i ) dst[i] = src[i] * g;
        return;
    }

    // The ramp crosses this span: per-sample, and exact.
    for( length_t i = 0; i < n; ++i ) {
        dst[i] = src[i] * (sample_t) factorAt( e, start + i );
    }
}

// ------------------------------------------------------------ the wide render

length_t twGainStage::renderPageWide( twOutputPage &page, length_t frames,
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
    const Envelope e = envelope();

    // 4.4 rule 2: take the producer's whole PAGE and pick channels ourselves.
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
        // Rule 1's clamp: a narrower producer plays on every channel.
        const sample_t *s = src->channelPtr( twPageClampChannel( *src, c ) );
        applyGain( s, dst, m, page.startPosition, e );
        if( m < n ) std::fill( dst + m, dst + n, 0.0f );
    }

    return n;
}

// ------------------------------------------------------------ the legacy pull

// Also the width-1 render: the base renderFrames() forwards here, exactly as it
// does for twRewire, so a mono track's chain grows one transparent copy and
// nothing else.
//
// KNOWN NARROWING, recorded rather than papered over: a plug pull is MONO by
// construction (proposal 36 4.4 rule 1), so only channel 0 of the producer can
// reach this seam. That is the same narrowing twPluginInsert::calcOutputTo
// carries, and it is why the wide path above exists.
//
// The mute RAMP needs a position, and a streaming pull has no page identity.
// renderPos_ is used: authoritative when the pull came from freezePage_nolock
// (which seeks first), and 0 for a free-running pull -- where the anchor is
// kRampImmediate in every path the app builds today anyway.
length_t twGainStage::calcOutputTo( IOVector& dest, idx_t idx )
{
    if( state_.load( std::memory_order_acquire ) == ComponentState::ZOMBIE ) {
        return dest.fillSilence( 0, dest.length() );
    }

    const length_t n = dest.length();
    if( n <= 0 ) return 0;

    std::shared_ptr<twLatchOutput> plug;
    {
        std::lock_guard<std::mutex> lock( mutex() );
        if( idx != 0 || pInputPlugs_.empty() || !pInputPlugs_[0] ) {
            return dest.fillSilence( 0, n );
        }
        plug = pInputPlugs_[0];
    }   // lock RELEASED before the pull into the producer

    std::vector<sample_t> buffer( (std::size_t) n, 0.0f );
    const length_t got = static_cast<twLatchStreamingOutput *>( plug.get() )
                             ->readStreamingData( buffer.data(), n );
    if( got <= 0 ) return dest.fillSilence( 0, n );

    const Envelope e = envelope();
    applyGain( buffer.data(), buffer.data(), got,
               renderPos_.load( std::memory_order_relaxed ), e );

    return dest.copyFrom( IOVector::CreateFromBuffer( buffer.data(), got ), 0, got );
}

// ------------------------------------------------------------------- teardown

void twGainStage::teardown()
{
    state_.store( ComponentState::ZOMBIE, std::memory_order_release );

    if( auto parent = parentComponent_.lock() ) {
        if( myInputIndex_ >= 0 ) {
            parent->removeInput( myInputIndex_ );
        }
    }

    std::vector<std::shared_ptr<twComponent> > depsCopy;
    {
        std::lock_guard<std::mutex> lock( mutex() );
        depsCopy = dependents_;
    }
    for( auto dep : depsCopy ) {
        if( dep ) dep->onDependencyTeardown( shared_from_this() );
    }

    // CASCADE UPSTREAM. twRewire::teardown() tears down whatever feeds it, and
    // that is how a track's whole chain gets torn down; standing between the
    // rewire and the chain, this component has to keep that cascade unbroken or
    // the plugin chain and the trackmix would never be told at all.
    std::vector<std::shared_ptr<twComponent> > inputsCopy;
    {
        std::lock_guard<std::mutex> lock( mutex() );
        for( const std::shared_ptr<twLatchOutput> &plug : pInputPlugs_ ) {
            if( !plug ) continue;
            std::shared_ptr<twComponent> comp = plug->getParentLatch().getComponent();
            if( comp ) inputsCopy.push_back( comp );
        }
    }
    for( auto &input : inputsCopy ) {
        input->teardown();
    }
}
