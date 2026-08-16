#include "tw/plugins/twplugininsert.h"
#include "tw/plugins/twplugin.h"
#include "tw/plugins/twpluginslotproc.h"
#include "tw/graph/tw303aenv.h"
#include "tw/pages/io_vector.h"
#include "tw/core/twlog.h"
#include <algorithm>
#include <cstdint>
#include <cstring>

namespace audio {

twPluginInsert::twPluginInsert( tw303aEnvironment &env,
                               std::shared_ptr<twPluginSlotProcessor> processor )
    : twComponent( env ), proc_( std::move( processor ) )
{
    // allocPlugs() / createOutputLatches() run from init(), which is also where
    // this insert registers itself with the processor (shared_from_this() is not
    // available yet here).
}

twPluginInsert::twPluginInsert( tw303aEnvironment &env,
                               std::unique_ptr<twPlugin> plugin )
    : twComponent( env )
{
    // Standalone form: wrap the already-built plugin in a private processor.
    // The channel count is the plugin's own input count, so a 2->2 plugin sees a
    // coherent pair — which is what the host-side chunking gate in plugins_test
    // exercises.
    twPluginIoLayout io{};
    if( plugin ) io = plugin->ioLayout();

    // The factory hands out the pre-built instance exactly once; a second call
    // (which only the dual-mono mapping makes, and this shape never selects it)
    // returns nullptr rather than a silent duplicate.
    auto held = std::make_shared<std::unique_ptr<twPlugin>>( std::move( plugin ) );
    proc_ = std::make_shared<twPluginSlotProcessor>(
        env,
        [held]() -> std::unique_ptr<twPlugin> {
            if( *held ) return std::move( *held );
            return nullptr;
        },
        io );
    proc_->setChannelCount( std::max<idx_t>( 1, (idx_t)io.audioInputs ) );
}

twPluginInsert::~twPluginInsert() = default;

idx_t twPluginInsert::getOutputChannels() const
{
    if( !proc_ ) return 1;
    const idx_t n = proc_->channelCount();
    return n > 0 ? n : 1;
}

void twPluginInsert::init()
{
    twComponent::init();

    // Registering here (not in the constructor) because the processor holds the
    // insert WEAKLY and therefore needs a real shared_ptr, which only exists once
    // make_shared has returned.
    if( proc_ )
        proc_->attachTap( std::static_pointer_cast<twPluginInsert>( shared_from_this() ) );
}

void twPluginInsert::createOutputLatches()
{
    // ONE port: the channel dimension is in the page, not in the patch bay.
    pOutputLatches_.resize( 1 );
    pOutputLatches_[0] = std::make_shared<twStreamingLatch>( shared_from_this(), 0, 4096 );
}

// ------------------------------------------------------------------ controls

void twPluginInsert::setBypass( bool bypass )
{
    // Forwarded to the shared processor, so the page invalidation that makes the
    // toggle audible happens in exactly one place.
    if( proc_ ) proc_->setBypass( bypass );
}

bool twPluginInsert::getBypass() const
{
    return proc_ ? proc_->bypass() : false;
}

twPlugin *twPluginInsert::getPlugin() const
{
    return proc_ ? proc_->plugin() : nullptr;
}

void twPluginInsert::reset()
{
    // Deliberately does NOT reset the plugin. twComponent::freezePage_nolock
    // calls reset() on every page discontinuity; whether the DSP may actually
    // continue is the processor's own lastEnd_ bookkeeping, which knows the
    // POSITIONS this one does not.
}

int twPluginInsert::seekTo( offset_t offset )
{
    // freezePage_nolock() calls seekTo(page->startPosition) immediately before
    // the render, INSIDE the component's cursorMutex_ — which is how the render
    // learns the page position its signature does not carry.
    //
    // The seek is deliberately NOT forwarded upstream: this insert reads its
    // producer positionally (fetchInputPage(0, pageStart)), so moving the
    // producer's cursor from here would only race the producer's own freeze.
    // twPluginChain::seekTo follows the same rule.
    renderPos_.store( offset, std::memory_order_relaxed );
    return 0;
}

// ---------------------------------------------------------------- upstream

length_t twPluginInsert::pullInputStreaming( sample_t *dst, length_t len )
{
    if( !dst || len <= 0 ) return 0;

    std::shared_ptr<twLatchOutput> plug;
    {
        std::lock_guard<std::mutex> lock( mutex() );
        if( !pInputPlugs_.empty() ) plug = pInputPlugs_[0];
    }                                   // <-- lock released BEFORE the pull
    if( !plug ) return 0;

    auto *input = static_cast<twLatchStreamingOutput *>( plug.get() );
    return input->readStreamingData( dst, len );
}

// ------------------------------------------------------------------ the render

// The shared core. Gather every channel of the upstream PAGE (§4.4 rule 2, with
// rule 1's clamp so a narrower producer plays on every channel), then ONE
// process() sweep over all of them.
//
// The gather is a copy rather than a set of pointers into the producer's page,
// for one reason: the page may be SHORTER than the render (a clip running out
// mid-page), and zero-padding the tail would mean writing into somebody else's
// page. It replaces the processor's old per-bus busIn_/busOut_ pair, so the
// total scratch across a slot went down, not up.
length_t twPluginInsert::renderCore_( sample_t **outCh, idx_t nCh,
                                      offset_t startPos, length_t frames,
                                      bool positional )
{
    if( !outCh || nCh <= 0 || frames <= 0 || !proc_ ) return 0;

    const std::size_t stride = (std::size_t)frames;
    if( gatherStride_ != stride || gather_.size() < (std::size_t)nCh * stride ) {
        gather_.assign( (std::size_t)nCh * stride, 0.0f );
        gatherStride_ = stride;
    }
    if( (idx_t)inPtrs_.size() < nCh ) inPtrs_.resize( (std::size_t)nCh );

    std::shared_ptr<twOutputPage> up = fetchInputPage( 0, startPos );
    const bool haveUp = up && up->validAspects != 0 && up->startPosition == startPos;
    const length_t m = haveUp
        ? std::min<length_t>( frames, (length_t)up->validFrames )
        : 0;

    for( idx_t c = 0; c < nCh; ++c ) {
        sample_t *dst = gather_.data() + (std::size_t)c * stride;
        if( m > 0 ) {
            const sample_t *s = up->channelPtr( twPageClampChannel( *up, c ) );
            std::copy( s, s + m, dst );
        }
        if( m < frames ) std::fill( dst + m, dst + frames, 0.0f );
        inPtrs_[c] = dst;
    }

    proc_->render( inPtrs_.data(), outCh, frames, startPos, positional,
                   env.getSRate() );
    return frames;
}

length_t twPluginInsert::renderPageWide( twOutputPage &page, length_t frames,
                                         const sample_t * /*input*/,
                                         length_t /*inputLength*/ )
{
    if( state_.load( std::memory_order_acquire ) == ComponentState::ZOMBIE ) {
        page.fillSilence();
        return 0;
    }

    length_t n = frames;
    if( n > (length_t)page.channelFrames() ) n = (length_t)page.channelFrames();
    if( n <= 0 ) return 0;

    const idx_t nCh = (idx_t)page.channels();
    std::vector<sample_t *> outCh( (std::size_t)nCh );
    for( idx_t c = 0; c < nCh; ++c ) outCh[c] = page.channelPtr( c );

    const length_t got = renderCore_( outCh.data(), nCh, page.startPosition, n, true );
    if( got <= 0 ) {
        page.fillSilence();
        return 0;
    }

    // A full page is always claimed, exactly as the pre-M3 insert did: the chain
    // is not the length authority (the render session and the project duration
    // are), and a plugin may legitimately produce tail past its input.
    return n;
}

length_t twPluginInsert::renderFrames( sample_t *output, length_t length,
                                       const sample_t * /*input*/,
                                       length_t /*inputLength*/,
                                       idx_t idx )
{
    if( !output || length <= 0 || idx != 0 || !proc_ ) return 0;

    const offset_t startPos = renderPos_.load( std::memory_order_relaxed );
    sample_t *outCh[1] = { output };
    if( renderCore_( outCh, 1, startPos, length, true ) <= 0 ) {
        std::fill( output, output + length, 0.0f );
    }
    return length;
}

// ------------------------------------------------------------ legacy pull path

// The streaming pull (twComponent::calcOutputTo). It has no page identity, so
// it cannot claim DSP continuity and passes positional = false.
//
// KNOWN NARROWING, recorded rather than papered over: a plug pull is MONO by
// construction (§4.4 rule 1), so only channel 0 of the input can reach the
// plugin here and the other channels are fed silence. Before B4 the insert had
// one plug per bus and could gather them all. Nothing in the app reaches this
// path — every consumer of a chain goes through freezePage — and a channel-
// coherent plugin driven from a mono seam has no better answer available.
length_t twPluginInsert::calcOutputTo( IOVector& dest, idx_t port )
{
    if( state_.load( std::memory_order_acquire ) == ComponentState::ZOMBIE )
        return dest.fillSilence( 0, dest.length() );

    if( port != 0 || !proc_ )
        return dest.fillSilence( 0, dest.length() );

    const length_t n = dest.length();
    if( n <= 0 ) return 0;

    const idx_t nCh = getOutputChannels();

    std::vector<sample_t> in( (std::size_t)nCh * (std::size_t)n, 0.0f );
    std::vector<sample_t> out( (std::size_t)nCh * (std::size_t)n, 0.0f );
    std::vector<const sample_t *> inPtrs( (std::size_t)nCh );
    std::vector<sample_t *>       outPtrs( (std::size_t)nCh );
    for( idx_t c = 0; c < nCh; ++c ) {
        inPtrs[c]  = in.data()  + (std::size_t)c * (std::size_t)n;
        outPtrs[c] = out.data() + (std::size_t)c * (std::size_t)n;
    }

    const length_t got = pullInputStreaming( in.data(), n );
    if( got < n ) std::fill( in.begin() + std::max<length_t>( got, 0 ), in.begin() + n, 0.0f );

    proc_->render( inPtrs.data(), outPtrs.data(), n, 0, false, env.getSRate() );

    return dest.copyFrom( IOVector::CreateFromBuffer( out.data(), n ), 0, n );
}

// --------------------------------------------------------------- freeze path

std::shared_ptr<twOutputPage> twPluginInsert::freezePage(
    offset_t startPos,
    const sample_t *inputData,
    uint64_t inputOffset,
    length_t inputLength,
    int sampleRate,
    std::shared_ptr<twOutputPage> previousPage )
{
    if( state_.load( std::memory_order_acquire ) == ComponentState::ZOMBIE ) {
        // WIDTH (proposal 36 §7 trap 19, fixed by B4): this override allocates
        // its own pages — here and in the preview path below — and so BYPASSES
        // the width wiring in twComponent::freezePage. Both now carry
        // getOutputChannels(), because the render fork is on the width of the
        // PAGE and a declared-4 component handing itself a width-1 page would
        // render channel 0 and publish it with no refusal.
        auto silencePage =
            std::make_shared<twOutputPage>( (std::uint16_t)getOutputChannels() );
        silencePage->setValidFrames( 0 );
        return silencePage;
    }

    // Preview freezes do not touch the plugin (CONTRACT invariant 6).
    //
    // freezePreviewPage() renders the same graph at a REDUCED rate (1 kHz
    // today) purely to get a waveform envelope. Honouring that rate would
    // re-prepare() — for CLAP, re-activate and reallocate — on every waveform
    // redraw, from a revalidator worker, while playback may be rendering the
    // same instance at the project rate. The envelope does not need the effect,
    // so the preview path forwards the upstream page unchanged. The
    // authoritative freeze always passes env.getSRate()
    // (twComponent::freezePageWithInputs), which is what makes this comparison
    // exact rather than a heuristic. The preview page is deliberately NOT
    // entered into outputPages_ — it is at the wrong rate for anyone else.
    if( sampleRate > 0 && sampleRate != env.getSRate() ) {
        const idx_t nCh = getOutputChannels();
        auto page = std::make_shared<twOutputPage>( (std::uint16_t)nCh );
        page->setStartPosition( startPos );
        page->setValidFrames( 0 );

        std::shared_ptr<twLatchOutput> plug;
        {
            std::lock_guard<std::mutex> lock( mutex() );
            if( !pInputPlugs_.empty() ) plug = pInputPlugs_[0];
        }
        std::shared_ptr<twComponent> producer =
            plug ? plug->getParentLatch().getComponent() : nullptr;
        if( producer ) {
            std::shared_ptr<twOutputPage> up = producer->freezePage(
                startPos, inputData, inputOffset, inputLength, sampleRate,
                previousPage );
            if( up ) {
                const length_t n = std::min<length_t>(
                    (length_t)up->validFrames,
                    std::min<length_t>( inputLength,
                                        (length_t)page->channelFrames() ) );
                if( n > 0 ) {
                    for( idx_t c = 0; c < nCh; ++c ) {
                        std::memcpy( page->channelPtr( c ),
                                     up->channelPtr( twPageClampChannel( *up, c ) ),
                                     (std::size_t)n * sizeof( sample_t ) );
                    }
                }
                page->setValidFrames( (uint32_t)std::max<length_t>( n, 0 ) );
            }
        }
        page->contentEpoch.store( contentEpochNow() );
        page->setValidAspects( twAspectPreview );
        return page;
    }

    // The authoritative path: let the base class own page identity, the cache,
    // the content-epoch stamping, the RT-thread guard, the stale-predecessor
    // fallback and the readiness gate. The render itself lands in
    // renderPageWide()/renderFrames(), which the base calls with no lock held.
    return twComponent::freezePage( startPos, inputData, inputOffset,
                                    inputLength, sampleRate, previousPage );
}

void twPluginInsert::teardown()
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

    // An insert has no children. The processor outlives it only as long as the
    // model slot still holds it.
}

}  // namespace audio
