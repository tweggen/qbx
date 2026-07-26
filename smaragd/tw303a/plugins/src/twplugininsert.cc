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
                               std::shared_ptr<twPluginSlotProcessor> processor,
                               idx_t busIndex )
    : twComponent( env ), proc_( std::move( processor ) ),
      busIndex_( busIndex < 0 ? 0 : busIndex )
{
    // allocPlugs() / createOutputLatches() run from init(), which is also where
    // this tap registers itself with the processor (shared_from_this() is not
    // available yet here).
}

twPluginInsert::twPluginInsert( tw303aEnvironment &env,
                               std::unique_ptr<twPlugin> plugin )
    : twComponent( env ), busIndex_( 0 )
{
    // Standalone form: wrap the already-built plugin in a private processor.
    // The bus count is the plugin's own input count, so a 2->2 plugin still
    // sees a coherent pair (this tap then serves bus 0) — which is what the
    // host-side chunking gate in plugins_test exercises.
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
    proc_->setBusCount( std::max<idx_t>( 1, (idx_t)io.audioInputs ) );
}

twPluginInsert::~twPluginInsert() = default;

void twPluginInsert::init()
{
    twComponent::init();

    // Registering here (not in the constructor) because the processor holds the
    // tap WEAKLY and therefore needs a real shared_ptr, which only exists once
    // make_shared has returned. A tap that is never init()ed simply never
    // supplies audio for its bus — the processor gathers silence for it.
    if( proc_ )
        proc_->attachTap( busIndex_,
                          std::static_pointer_cast<twPluginInsert>( shared_from_this() ) );
}

void twPluginInsert::createOutputLatches()
{
    // One mono wire: exactly one output latch.
    pOutputLatches_.resize( 1 );
    pOutputLatches_[0] = std::make_shared<twStreamingLatch>( shared_from_this(), 0, 4096 );
}

// ------------------------------------------------------------------ controls

void twPluginInsert::setBypass( bool bypass )
{
    // Forwarded to the shared processor, so the flag reaches every bus of the
    // slot at once — and so the page invalidation that makes the toggle audible
    // happens in exactly one place.
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

void twPluginInsert::resetBlock()
{
    if( proc_ ) proc_->resetBlock();
}

void twPluginInsert::reset()
{
    // Deliberately does NOT reset the plugin. twComponent::freezePage_nolock
    // calls reset() on every page discontinuity, for EVERY tap of the slot; a
    // reset here would wipe the DSP state the sibling tap's render just
    // produced (the taps render one shared instance). Position continuity is
    // the processor's own business — it resets when a page does not start where
    // the last one ended.
}

int twPluginInsert::seekTo( offset_t offset )
{
    // freezePage_nolock() calls seekTo(page->startPosition) immediately before
    // renderFrames(), INSIDE the component's cursorMutex_ — which is how
    // renderFrames() learns the page position its signature does not carry.
    //
    // The seek is deliberately NOT forwarded upstream any more: this tap reads
    // its producer through requestPage(startPos, ...), which is positional by
    // construction, so moving the producer's cursor from here would only race
    // the producer's own freeze. twPluginChain::seekTo still seeks the chain's
    // input plugs (the track mix) and every tap.
    renderPos_.store( offset, std::memory_order_relaxed );
    return 0;
}

// ---------------------------------------------------------------- upstream

// Snapshot the producing component of our single input under a BRIEF lock and
// return it with the lock released. See CONTRACT invariant 13: the caller
// (the processor, holding the processor mutex on behalf of whichever sibling
// tap asked first) must never end up waiting for a tap mutex that is itself
// waiting for the processor mutex.
static std::shared_ptr<twComponent> producerOf( const std::shared_ptr<twLatchOutput> &plug )
{
    if( !plug ) return nullptr;
    return plug->getParentLatch().getComponent();
}

length_t twPluginInsert::pullUpstreamPage( offset_t startPos, length_t len,
                                           int sampleRate, sample_t *dst )
{
    if( !dst || len <= 0 ) return 0;

    std::shared_ptr<twLatchOutput> plug;
    {
        std::lock_guard<std::mutex> lock( mutex() );
        if( !pInputPlugs_.empty() ) plug = pInputPlugs_[0];
    }                                   // <-- lock released BEFORE the pull
    std::shared_ptr<twComponent> producer = producerOf( plug );
    if( !producer ) return 0;

    // Only the immediately preceding page may chain DSP state; anything else is
    // a discontinuity the producer answers with reset() + seekTo(). Same rule,
    // and same per-reader hint, as twStreamingLatch::copyData.
    std::shared_ptr<twOutputPage> chainFrom;
    if( upstreamHint_ && upstreamHint_->validAspects != 0 &&
        upstreamHint_->contentEpoch.load() >= producer->contentEpochNow() &&
        upstreamHint_->startPosition + upstreamHint_->validFrames == startPos ) {
        chainFrom = upstreamHint_;
    }

    // requestPage(), not freezePage(): the proposal-19 dedup front door, so two
    // drivers demanding the same producer page collapse to one render.
    std::shared_ptr<twOutputPage> page = producer->requestPage(
        startPos, nullptr, 0, len, sampleRate, chainFrom );
    upstreamHint_ = page;

    if( !page || page->validAspects == 0 || page->startPosition != startPos )
        return 0;

    const length_t n = std::min<length_t>( len, (length_t)page->validFrames );
    if( n > 0 )
        std::memcpy( dst, page->samples.data(), (std::size_t)n * sizeof( sample_t ) );
    return n;
}

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

// ------------------------------------------------------------ legacy pull path

length_t twPluginInsert::calcOutputTo( IOVector& dest, idx_t port )
{
    if( state_.load( std::memory_order_acquire ) == ComponentState::ZOMBIE )
        return dest.fillSilence( 0, dest.length() );

    if( port != 0 || !proc_ )
        return dest.fillSilence( 0, dest.length() );

    // NOTE: no component mutex here. The processor serializes the shared render
    // itself, and holding this tap's mutex across it would violate CONTRACT
    // invariant 13 exactly as it would in the freeze path.
    std::vector<sample_t> buffer( (std::size_t)dest.length() );
    const length_t n = proc_->blockFor( busIndex_, buffer.data(), dest.length() );
    if( n <= 0 ) return dest.fillSilence( 0, dest.length() );
    return dest.copyFrom( IOVector::CreateFromBuffer( buffer.data(), n ), 0, n );
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
        auto silencePage = std::make_shared<twOutputPage>();
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
        auto page = std::make_shared<twOutputPage>();
        page->setStartPosition( startPos );
        page->setValidFrames( 0 );

        std::shared_ptr<twLatchOutput> plug;
        {
            std::lock_guard<std::mutex> lock( mutex() );
            if( !pInputPlugs_.empty() ) plug = pInputPlugs_[0];
        }
        std::shared_ptr<twComponent> producer = producerOf( plug );
        if( producer ) {
            std::shared_ptr<twOutputPage> up = producer->freezePage(
                startPos, inputData, inputOffset, inputLength, sampleRate,
                previousPage );
            if( up ) {
                const length_t n = std::min<length_t>(
                    (length_t)up->validFrames,
                    std::min<length_t>( inputLength,
                                        (length_t)twOutputPage::FRAME_CAPACITY ) );
                if( n > 0 )
                    std::memcpy( page->samples.data(), up->samples.data(),
                                 (std::size_t)n * sizeof( sample_t ) );
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
    // renderFrames(), which the base calls with no lock held.
    return twComponent::freezePage( startPos, inputData, inputOffset,
                                    inputLength, sampleRate, previousPage );
}

length_t twPluginInsert::renderFrames( sample_t *output, length_t length,
                                       const sample_t * /*input*/,
                                       length_t /*inputLength*/,
                                       idx_t idx )
{
    if( !output || length <= 0 || idx != 0 || !proc_ ) return 0;

    const offset_t startPos = renderPos_.load( std::memory_order_relaxed );
    const int      rate     = env.getSRate();

    // The shared render. No component mutex is held here — the processor mutex
    // is the only lock across the gather, which is what keeps bus 0 gathering
    // bus 1 from deadlocking against bus 1's own freeze (CONTRACT 13).
    std::shared_ptr<const twPluginSlotProcessor::Output> out =
        proc_->pageFor( busIndex_, startPos, length, rate );

    if( !out || busIndex_ >= (idx_t)out->bus.size() ) {
        std::fill( output, output + length, 0.0f );
        return length;
    }

    const std::vector<sample_t> &src = out->bus[busIndex_];
    const length_t n = std::min<length_t>( length, (length_t)src.size() );
    if( n > 0 )
        std::memcpy( output, src.data(), (std::size_t)n * sizeof( sample_t ) );
    if( n < length )
        std::fill( output + n, output + length, 0.0f );

    // A full page is always claimed, exactly as the pre-M3 insert did: the
    // chain is not the length authority (the render session and the project
    // duration are), and a plugin may legitimately produce tail past its input.
    return length;
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

    // A tap has no children. The processor outlives it only as long as a
    // sibling tap (or the model slot) still holds it.
}

}  // namespace audio
