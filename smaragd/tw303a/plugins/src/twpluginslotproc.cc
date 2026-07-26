#include "tw/plugins/twpluginslotproc.h"

#include "tw/core/twlog.h"
#include "tw/graph/tw303aenv.h"
#include "tw/plugins/twplugininsert.h"

#include <algorithm>
#include <cstring>

namespace audio {

twPluginSlotProcessor::twPluginSlotProcessor( tw303aEnvironment &env,
                                             Factory factory,
                                             const twPluginIoLayout &declaredIo )
    : env_( env ), factory_( std::move( factory ) ), declaredIo_( declaredIo )
{
    cache_.resize( kCacheEntries );
}

twPluginSlotProcessor::~twPluginSlotProcessor() = default;

// ---------------------------------------------------------------- configuration

void twPluginSlotProcessor::setBusCount( idx_t nBuses )
{
    if( nBuses < 0 ) nBuses = 0;

    std::lock_guard<std::mutex> lock( mutex_ );
    if( nBuses == nBuses_ && !instances_.empty() ) return;

    nBuses_ = nBuses;
    if( (idx_t)taps_.size() < nBuses_ ) taps_.resize( nBuses_ );
    rebuild_nolock();
}

idx_t twPluginSlotProcessor::busCount() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return nBuses_;
}

void twPluginSlotProcessor::attachTap( idx_t bus,
                                       const std::shared_ptr<twPluginInsert> &tap )
{
    if( bus < 0 ) return;
    std::lock_guard<std::mutex> lock( mutex_ );
    if( (idx_t)taps_.size() <= bus ) taps_.resize( bus + 1 );
    taps_[bus] = tap;
}

twPluginSlotMode twPluginSlotProcessor::mode() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return mode_;
}

twPluginSlotState twPluginSlotProcessor::state() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return state_;
}

void twPluginSlotProcessor::setBypass( bool bypass )
{
    if( bypass_.exchange( bypass, std::memory_order_acq_rel ) == bypass ) return;
    // A cached page rendered with the old flag would otherwise be served
    // unchanged and the toggle would be inaudible.
    bumpParamEpoch();
}

void twPluginSlotProcessor::bumpParamEpoch()
{
    std::lock_guard<std::mutex> lock( mutex_ );
    bumpParamEpoch_nolock();
}

// Caller must hold mutex_. Two things have to move together: the cache key
// (or the processor serves the page it rendered before the edit) and every
// tap's content epoch (or the taps serve THEIR cached pages instead of asking
// again). twComponent::bumpContentEpoch() is a lock-free atomic increment, so
// calling it under mutex_ introduces no lock ordering.
void twPluginSlotProcessor::bumpParamEpoch_nolock()
{
    paramEpoch_.fetch_add( 1, std::memory_order_acq_rel );
    for( const std::weak_ptr<twPluginInsert> &w : taps_ ) {
        if( std::shared_ptr<twPluginInsert> t = w.lock() )
            t->bumpContentEpoch();
    }
}

twPlugin *twPluginSlotProcessor::plugin() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return instances_.empty() ? nullptr : instances_[0].get();
}

std::vector<twPlugin *> twPluginSlotProcessor::plugins() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    std::vector<twPlugin *> out;
    out.reserve( instances_.size() );
    for( const std::unique_ptr<twPlugin> &p : instances_ )
        if( p ) out.push_back( p.get() );
    return out;
}

// --------------------------------------------------------------- the policy

// Caller must hold mutex_. Derives the channel-mismatch mapping of proposal 08
// §Layer 3 and materializes exactly the instances it needs.
void twPluginSlotProcessor::rebuild_nolock()
{
    instances_.clear();
    preparedRate_ = 0;
    mode_         = twPluginSlotMode::Transparent;
    state_        = twPluginSlotState::Active;
    haveLastEnd_  = false;
    std::fill( cache_.begin(), cache_.end(), std::shared_ptr<const Output>{} );
    bumpParamEpoch_nolock();

    if( nBuses_ <= 0 || !factory_ ) return;

    // One instance is created FIRST so the mapping is decided against the
    // plugin's own reported layout, not against a descriptor that a stale
    // project file or an out-of-date scan cache may disagree with.
    std::unique_ptr<twPlugin> first = factory_();
    if( !first ) {
        // No backend, no module, or a refused descriptor. The slot stays in the
        // graph and stays transparent — M4 turns this into a persisted
        // missing-plugin placeholder with the descriptor's declared I/O.
        state_ = twPluginSlotState::Missing;
        TW_LOGW( "plugins", "[slot] could not instantiate the plugin (declared %u in / "
                 "%u out); the slot loads transparent",
                 (unsigned)declaredIo_.audioInputs, (unsigned)declaredIo_.audioOutputs );
        return;
    }

    const twPluginIoLayout io = first->ioLayout();
    const idx_t nIn  = (idx_t)io.audioInputs;
    const idx_t nOut = (idx_t)io.audioOutputs;

    if( nIn == nBuses_ && nOut == nBuses_ ) {
        // N -> N: the normal case (2->2 on a stereo track, 1->1 on a mono one).
        mode_ = twPluginSlotMode::Direct;
        instances_.push_back( std::move( first ) );
    } else if( nIn == 1 && nOut == 1 && nBuses_ > 1 ) {
        // Dual-mono: run the plugin independently per bus (L->L, R->R). The
        // image survives; channel-linked internal state is NOT shared, which is
        // correct for EQ/filter/distortion and is why this needs N instances —
        // and therefore a factory rather than a single instance.
        mode_ = twPluginSlotMode::DualMono;
        instances_.push_back( std::move( first ) );
        for( idx_t b = 1; b < nBuses_; ++b ) {
            std::unique_ptr<twPlugin> extra = factory_();
            if( !extra ) {
                // A partial dual-mono chain would silence some buses; refuse it.
                TW_LOGW( "plugins", "[slot] dual-mono needs %d instances but the factory "
                         "produced only %d; the slot loads transparent",
                         (int)nBuses_, (int)instances_.size() );
                instances_.clear();
                mode_  = twPluginSlotMode::Transparent;
                state_ = twPluginSlotState::Missing;
                return;
            }
            instances_.push_back( std::move( extra ) );
        }
    } else if( nIn == 2 && nOut == 2 && nBuses_ == 1 ) {
        // A stereo plugin on a mono wire: feed the one input to both plugin
        // inputs and average the two outputs back down.
        mode_ = twPluginSlotMode::MonoFold;
        instances_.push_back( std::move( first ) );
    } else {
        // >2 channels, or asymmetric in != out: no auto-mix. Until a routing
        // matrix exists, guessing would be worse than being transparent.
        mode_  = twPluginSlotMode::Transparent;
        state_ = twPluginSlotState::Unsupported;
        if( !loggedUnsupported_ ) {
            loggedUnsupported_ = true;
            TW_LOGW( "plugins", "[slot] no defined mapping for a %d-in / %d-out plugin on "
                     "%d bus(es) (proposal 08 §Layer 3); the slot is UNSUPPORTED and "
                     "loads transparent", (int)nIn, (int)nOut, (int)nBuses_ );
        }
        return;
    }

    // prepare() is reached from setBusCount(), i.e. from the UI thread — which
    // is where CLAP says activate() belongs.
    const int rate = env_.getSRate();
    if( rate > 0 ) {
        for( const std::unique_ptr<twPlugin> &p : instances_ )
            p->prepare( (std::uint32_t)rate, (std::uint32_t)kChunkFrames );
        preparedRate_ = rate;
    }
}

// ----------------------------------------------------------------- scratch

void twPluginSlotProcessor::ensureScratch_nolock( length_t len )
{
    const std::size_t want = (std::size_t)std::max<length_t>( len, kChunkFrames );

    if( (idx_t)busIn_.size()  != nBuses_ ) busIn_.resize( nBuses_ );
    if( (idx_t)busOut_.size() != nBuses_ ) busOut_.resize( nBuses_ );
    for( std::vector<sample_t> &b : busIn_ )
        if( b.size() < want ) b.resize( want );
    for( std::vector<sample_t> &b : busOut_ )
        if( b.size() < want ) b.resize( want );

    if( mode_ == twPluginSlotMode::MonoFold ) {
        if( foldOut_.size() != 2 ) foldOut_.resize( 2 );
        for( std::vector<sample_t> &b : foldOut_ )
            if( b.size() < want ) b.resize( want );
    }

    const std::size_t nIn  = instances_.empty() ? 0
        : (std::size_t)instances_[0]->ioLayout().audioInputs;
    const std::size_t nOut = instances_.empty() ? 0
        : (std::size_t)instances_[0]->ioLayout().audioOutputs;
    if( inPtrs_.size()  < nIn  ) inPtrs_.resize( nIn );
    if( outPtrs_.size() < nOut ) outPtrs_.resize( nOut );
}

void twPluginSlotProcessor::resetInstances_nolock()
{
    for( const std::unique_ptr<twPlugin> &p : instances_ )
        if( p ) p->reset();
}

// ------------------------------------------------------------- the DSP core

// Caller must hold mutex_. busIn_ holds len frames per bus; busOut_ receives
// len frames per bus. Chunked to kChunkFrames, advancing through the SAME
// buffers so plugin DSP state carries across chunks exactly as it would across
// callbacks in a live host (CONTRACT invariant 5).
void twPluginSlotProcessor::runChunked_nolock( length_t len )
{
    if( len <= 0 ) return;

    if( mode_ == twPluginSlotMode::Transparent || instances_.empty() ||
        bypass_.load( std::memory_order_acquire ) ) {
        for( idx_t b = 0; b < nBuses_; ++b )
            std::copy( busIn_[b].begin(), busIn_[b].begin() + len,
                       busOut_[b].begin() );
        return;
    }

    for( length_t off = 0; off < len; off += kChunkFrames ) {
        const length_t n = std::min<length_t>( kChunkFrames, len - off );

        switch( mode_ ) {
        case twPluginSlotMode::Direct: {
            for( idx_t c = 0; c < nBuses_; ++c ) {
                inPtrs_[c]  = busIn_[c].data()  + off;
                outPtrs_[c] = busOut_[c].data() + off;
            }
            instances_[0]->process( inPtrs_.data(), outPtrs_.data(), (std::uint32_t)n );
            break;
        }
        case twPluginSlotMode::DualMono: {
            for( idx_t b = 0; b < nBuses_; ++b ) {
                inPtrs_[0]  = busIn_[b].data()  + off;
                outPtrs_[0] = busOut_[b].data() + off;
                instances_[b]->process( inPtrs_.data(), outPtrs_.data(),
                                        (std::uint32_t)n );
            }
            break;
        }
        case twPluginSlotMode::MonoFold: {
            // Both plugin inputs read the one bus. Aliasing two const input
            // pointers at one buffer is safe by the twPlugin contract: process()
            // never writes through `in`.
            inPtrs_[0]  = busIn_[0].data() + off;
            inPtrs_[1]  = busIn_[0].data() + off;
            outPtrs_[0] = foldOut_[0].data() + off;
            outPtrs_[1] = foldOut_[1].data() + off;
            instances_[0]->process( inPtrs_.data(), outPtrs_.data(), (std::uint32_t)n );
            sample_t *dst = busOut_[0].data() + off;
            for( length_t i = 0; i < n; ++i )
                dst[i] = 0.5f * ( outPtrs_[0][i] + outPtrs_[1][i] );
            break;
        }
        case twPluginSlotMode::Transparent:
            break;   // handled above
        }
    }
}

// ------------------------------------------------------------------- caching

std::uint64_t twPluginSlotProcessor::stampNow_nolock() const
{
    // The key must move when EITHER the processor changes (bypass, parameter,
    // bus count) or any bus's upstream audio changes. The taps' own content
    // epochs carry the latter; both counters are monotonic, so their sum is too
    // and cannot alias.
    std::uint64_t stamp = paramEpoch_.load( std::memory_order_acquire );
    for( const std::weak_ptr<twPluginInsert> &w : taps_ ) {
        if( std::shared_ptr<twPluginInsert> t = w.lock() )
            stamp += t->contentEpochNow();
    }
    return stamp;
}

std::shared_ptr<const twPluginSlotProcessor::Output>
twPluginSlotProcessor::findCached_nolock( offset_t startPos, length_t len,
                                          std::uint64_t stamp ) const
{
    for( const std::shared_ptr<const Output> &o : cache_ ) {
        if( o && o->startPos == startPos && o->frames == len && o->stamp == stamp )
            return o;
    }
    return nullptr;
}

void twPluginSlotProcessor::publish_nolock( const std::shared_ptr<const Output> &out )
{
    cache_[cacheNext_] = out;
    cacheNext_ = ( cacheNext_ + 1 ) % cache_.size();
}

// ---------------------------------------------------------------- freeze path

std::shared_ptr<const twPluginSlotProcessor::Output>
twPluginSlotProcessor::pageFor( idx_t bus, offset_t startPos, length_t len,
                                int sampleRate )
{
    std::lock_guard<std::mutex> lock( mutex_ );

    if( len < 0 ) len = 0;
    const std::uint64_t stamp = stampNow_nolock();

    if( std::shared_ptr<const Output> hit = findCached_nolock( startPos, len, stamp ) )
        return hit;

    (void)bus;   // every bus is rendered; which one asked does not matter

    ensureScratch_nolock( len );

    // The plugin was activated for a sample rate. A genuine project rate change
    // must re-prepare it; this is observed from whichever thread renders next
    // (recorded debt: CLAP marks activate() [main-thread]).
    if( sampleRate > 0 && sampleRate != preparedRate_ && !instances_.empty() ) {
        for( const std::unique_ptr<twPlugin> &p : instances_ )
            p->prepare( (std::uint32_t)sampleRate, (std::uint32_t)kChunkFrames );
        preparedRate_ = sampleRate;
        haveLastEnd_  = false;
    }

    // Gather every bus's upstream audio through the sibling taps. This is where
    // CONTRACT invariant 13 lives: pullUpstreamPage() must not take the tap's
    // own component mutex, and no tap may hold its own mutex across pageFor(),
    // or bus 0 gathering bus 1 deadlocks against bus 1's own freeze.
    for( idx_t b = 0; b < nBuses_; ++b ) {
        sample_t *dst = busIn_[b].data();
        length_t  got = 0;
        if( b < (idx_t)taps_.size() ) {
            if( std::shared_ptr<twPluginInsert> t = taps_[b].lock() )
                got = t->pullUpstreamPage( startPos, len, sampleRate, dst );
        }
        if( got < len )
            std::fill( dst + std::max<length_t>( got, 0 ), dst + len, 0.0f );
    }

    // Stateful DSP: a page that does not start exactly where the last one ended
    // is a discontinuity the plugin cannot continue from.
    if( haveLastEnd_ && startPos != lastEnd_ ) {
        TW_LOGD( "plugins", "[slot] non-sequential page at %lld (expected %lld); "
                 "resetting plugin state",
                 (long long)startPos, (long long)lastEnd_ );
        resetInstances_nolock();
    }

    runChunked_nolock( len );

    lastEnd_     = startPos + (offset_t)len;
    haveLastEnd_ = true;

    auto out = std::make_shared<Output>();
    out->startPos = startPos;
    out->frames   = len;
    out->stamp    = stamp;
    out->bus.resize( nBuses_ );
    for( idx_t b = 0; b < nBuses_; ++b ) {
        out->bus[b].assign( busOut_[b].begin(), busOut_[b].begin() + len );
    }

    std::shared_ptr<const Output> ro = out;
    publish_nolock( ro );
    return ro;
}

// ------------------------------------------------------------ legacy pull path

void twPluginSlotProcessor::resetBlock()
{
    std::lock_guard<std::mutex> lock( mutex_ );
    blockProduced_ = false;
}

length_t twPluginSlotProcessor::blockFor( idx_t bus, sample_t *dst, length_t len )
{
    if( !dst || len <= 0 ) return 0;

    std::lock_guard<std::mutex> lock( mutex_ );
    if( bus < 0 || bus >= nBuses_ ) return 0;

    ensureScratch_nolock( len );

    if( !blockProduced_ ) {
        if( sampleRateChangedForPull_nolock() ) haveLastEnd_ = false;
        for( idx_t b = 0; b < nBuses_; ++b ) {
            sample_t *in  = busIn_[b].data();
            length_t  got = 0;
            if( b < (idx_t)taps_.size() ) {
                if( std::shared_ptr<twPluginInsert> t = taps_[b].lock() )
                    got = t->pullInputStreaming( in, len );
            }
            if( got < len )
                std::fill( in + std::max<length_t>( got, 0 ), in + len, 0.0f );
        }
        runChunked_nolock( len );
        blockProduced_ = true;
    }

    std::copy( busOut_[bus].begin(), busOut_[bus].begin() + len, dst );
    return len;
}

// Caller must hold mutex_. The pull path carries no sample rate of its own, so
// it uses the environment's — and re-prepares if the project rate moved.
bool twPluginSlotProcessor::sampleRateChangedForPull_nolock()
{
    const int rate = env_.getSRate();
    if( rate <= 0 || rate == preparedRate_ || instances_.empty() ) return false;
    for( const std::unique_ptr<twPlugin> &p : instances_ )
        p->prepare( (std::uint32_t)rate, (std::uint32_t)kChunkFrames );
    preparedRate_ = rate;
    return true;
}

}  // namespace audio
