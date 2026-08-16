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
}

twPluginSlotProcessor::~twPluginSlotProcessor() = default;

// ---------------------------------------------------------------- configuration

void twPluginSlotProcessor::setChannelCount( idx_t nChannels )
{
    if( nChannels < 0 ) nChannels = 0;

    std::lock_guard<std::mutex> lock( mutex_ );
    if( nChannels == nChannels_ && !instances_.empty() ) return;

    nChannels_ = nChannels;
    rebuild_nolock();
}

// Re-resolution after a rescan (proposal 08 M4). The insert holds this processor
// by shared_ptr and the DSP chain holds the insert, so a slot whose plugin
// appeared on disk must NOT be rebuilt by swapping the processor — that would
// mean re-wiring the chain. Handing it a new factory instead re-runs exactly
// what setChannelCount() derives, keeps the graph untouched, and stales the
// insert's pages through rebuild_nolock()'s bumpParamEpoch_nolock().
void twPluginSlotProcessor::setFactory( Factory factory )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    factory_ = std::move( factory );
    // Reset the once-per-slot log gate: the new factory may map differently, and
    // that verdict is worth one line again.
    loggedUnsupported_ = false;
    rebuild_nolock();
}

idx_t twPluginSlotProcessor::channelCount() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return nChannels_;
}

void twPluginSlotProcessor::attachTap( const std::shared_ptr<twPluginInsert> &tap )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    tap_ = tap;
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

// Caller must hold mutex_. The insert's frozen pages bake in what process()
// produced, so an edit that changes process() has to stale them or it is
// inaudible. twComponent::bumpContentEpoch() is a lock-free atomic increment, so
// calling it under mutex_ introduces no lock ordering.
void twPluginSlotProcessor::bumpParamEpoch_nolock()
{
    if( std::shared_ptr<twPluginInsert> t = tap_.lock() )
        t->bumpContentEpoch();
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
//
// PROPOSAL 36 B4 CHANGED THE NUMBER, NOT THE POLICY. nBuses_ became nChannels_
// — the width of the page the insert is handed rather than the count of
// parallel mono components a track was built from — and every branch below is
// otherwise the one proposal 08 settled. A user must not be able to hear this
// milestone through a mismatched plugin.
void twPluginSlotProcessor::rebuild_nolock()
{
    instances_.clear();
    preparedRate_ = 0;
    mode_         = twPluginSlotMode::Transparent;
    state_        = twPluginSlotState::Active;
    haveLastEnd_  = false;
    bumpParamEpoch_nolock();

    if( nChannels_ <= 0 ) return;

    // No backend, no module, or a refused descriptor: SUBSTITUTE the placeholder
    // (proposal 08 M4) rather than leaving the slot instance-less. The
    // placeholder reports the descriptor's DECLARED layout, so the mapping, the
    // instance count and the prepare() bookkeeping are the ones the real plugin
    // will get once it is installed — the slot is transparent, but it is not a
    // differently-shaped graph. `substituted` is what turns into the persisted
    // Missing state below.
    bool substituted = false;
    auto makeInstance = [this, &substituted]() -> std::unique_ptr<twPlugin> {
        std::unique_ptr<twPlugin> p = factory_ ? factory_() : nullptr;
        if( !p ) {
            p = createNullPlugin( declaredIo_ );
            substituted = true;
        }
        return p;
    };

    // One instance is created FIRST so the mapping is decided against the
    // plugin's own reported layout, not against a descriptor that a stale
    // project file or an out-of-date scan cache may disagree with.
    std::unique_ptr<twPlugin> first = makeInstance();
    if( substituted ) {
        TW_LOGW( "plugins", "[slot] could not instantiate the plugin (declared %u in / "
                 "%u out); the slot runs the transparent placeholder and is MISSING",
                 (unsigned)declaredIo_.audioInputs, (unsigned)declaredIo_.audioOutputs );
    }

    const twPluginIoLayout io = first->ioLayout();
    const idx_t nIn  = (idx_t)io.audioInputs;
    const idx_t nOut = (idx_t)io.audioOutputs;

    if( nIn == nChannels_ && nOut == nChannels_ ) {
        // N -> N: the normal case (2->2 on a stereo track, 1->1 on a mono one).
        mode_ = twPluginSlotMode::Direct;
        instances_.push_back( std::move( first ) );
    } else if( nIn == 1 && nOut == 1 && nChannels_ > 1 ) {
        // Dual-mono: run the plugin independently per channel (L->L, R->R). The
        // image survives; channel-linked internal state is NOT shared, which is
        // correct for EQ/filter/distortion and is why this needs N instances —
        // and therefore a factory rather than a single instance.
        mode_ = twPluginSlotMode::DualMono;
        instances_.push_back( std::move( first ) );
        for( idx_t b = 1; b < nChannels_; ++b ) {
            // makeInstance() never returns null (it falls back to the
            // placeholder), so a PARTIAL dual-mono chain — which used to silence
            // whole channels — is no longer reachable. A factory that produces
            // one real instance and then fails marks the whole slot Missing.
            instances_.push_back( makeInstance() );
        }
    } else if( nIn == 2 && nOut == 2 && nChannels_ == 1 ) {
        // A stereo plugin on a mono wire: feed the one input to both plugin
        // inputs and average the two outputs back down.
        mode_ = twPluginSlotMode::MonoFold;
        instances_.push_back( std::move( first ) );
    } else {
        // >2 channels, or asymmetric in != out: no auto-mix. Until a routing
        // matrix exists, guessing would be worse than being transparent.
        mode_  = twPluginSlotMode::Transparent;
        // Missing WINS over Unsupported: a substituted placeholder's layout is
        // whatever the saved descriptor claimed, so "the plugin is not here" is
        // both the cause and the actionable report — and it is what M5's Reload
        // affordance keys on.
        state_ = substituted ? twPluginSlotState::Missing
                             : twPluginSlotState::Unsupported;
        if( !loggedUnsupported_ ) {
            loggedUnsupported_ = true;
            TW_LOGW( "plugins", "[slot] no defined mapping for a %d-in / %d-out plugin on "
                     "%d channel(s) (proposal 08 §Layer 3); the slot is UNSUPPORTED and "
                     "loads transparent", (int)nIn, (int)nOut, (int)nChannels_ );
        }
        return;
    }

    if( substituted ) state_ = twPluginSlotState::Missing;

    // prepare() is reached from setChannelCount(), i.e. from the UI thread —
    // which is where CLAP says activate() belongs.
    const int rate = env_.getSRate();
    if( rate > 0 ) {
        for( const std::unique_ptr<twPlugin> &p : instances_ )
            p->prepare( (std::uint32_t)rate, (std::uint32_t)kChunkFrames );
        preparedRate_ = rate;
    }
}

// ----------------------------------------------------------------- scratch

// Caller must hold mutex_. The per-channel gather/result buffers used to live
// here (busIn_/busOut_, one page each per bus); B4 moved them to the caller,
// which now hands in its upstream page's channels and its own page's channels
// directly. What is left is the MonoFold fold-down pair — one CHUNK each,
// because the fold happens inside the chunk loop — and the pointer arrays
// process() is handed.
void twPluginSlotProcessor::ensureScratch_nolock()
{
    if( mode_ == twPluginSlotMode::MonoFold ) {
        if( foldOut_.size() != 2 ) foldOut_.resize( 2 );
        for( std::vector<sample_t> &b : foldOut_ )
            if( b.size() < (std::size_t)kChunkFrames ) b.resize( (std::size_t)kChunkFrames );
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

// Caller must hold mutex_. `in` and `out` are nChannels_ planar buffers of at
// least `len` frames. Chunked to kChunkFrames, advancing through the SAME
// buffers so plugin DSP state carries across chunks exactly as it would across
// callbacks in a live host (CONTRACT invariant 5).
void twPluginSlotProcessor::runChunked_nolock( const sample_t *const *in,
                                               sample_t **out, length_t len )
{
    if( len <= 0 || !in || !out ) return;

    if( mode_ == twPluginSlotMode::Transparent || instances_.empty() ||
        bypass_.load( std::memory_order_acquire ) ) {
        for( idx_t c = 0; c < nChannels_; ++c ) {
            if( in[c] && out[c] ) std::copy( in[c], in[c] + len, out[c] );
        }
        return;
    }

    for( length_t off = 0; off < len; off += kChunkFrames ) {
        const length_t n = std::min<length_t>( kChunkFrames, len - off );

        switch( mode_ ) {
        case twPluginSlotMode::Direct: {
            for( idx_t c = 0; c < nChannels_; ++c ) {
                inPtrs_[c]  = in[c]  + off;
                outPtrs_[c] = out[c] + off;
            }
            instances_[0]->process( inPtrs_.data(), outPtrs_.data(), (std::uint32_t)n );
            break;
        }
        case twPluginSlotMode::DualMono: {
            for( idx_t b = 0; b < nChannels_; ++b ) {
                inPtrs_[0]  = in[b]  + off;
                outPtrs_[0] = out[b] + off;
                instances_[b]->process( inPtrs_.data(), outPtrs_.data(),
                                        (std::uint32_t)n );
            }
            break;
        }
        case twPluginSlotMode::MonoFold: {
            // Both plugin inputs read the one channel. Aliasing two const input
            // pointers at one buffer is safe by the twPlugin contract: process()
            // never writes through `in`.
            inPtrs_[0]  = in[0] + off;
            inPtrs_[1]  = in[0] + off;
            outPtrs_[0] = foldOut_[0].data();
            outPtrs_[1] = foldOut_[1].data();
            instances_[0]->process( inPtrs_.data(), outPtrs_.data(), (std::uint32_t)n );
            sample_t *dst = out[0] + off;
            for( length_t i = 0; i < n; ++i )
                dst[i] = 0.5f * ( outPtrs_[0][i] + outPtrs_[1][i] );
            break;
        }
        case twPluginSlotMode::Transparent:
            break;   // handled above
        }
    }
}

// ---------------------------------------------------------------- the render

void twPluginSlotProcessor::render( const sample_t *const *in, sample_t **out,
                                    length_t len, offset_t startPos,
                                    bool positional, int sampleRate )
{
    if( len <= 0 ) return;

    std::lock_guard<std::mutex> lock( mutex_ );

    ensureScratch_nolock();

    // The plugin was activated for a sample rate. A genuine project rate change
    // must re-prepare it; this is observed from whichever thread renders next
    // (recorded debt: CLAP marks activate() [main-thread]).
    if( sampleRate > 0 && sampleRate != preparedRate_ && !instances_.empty() ) {
        for( const std::unique_ptr<twPlugin> &p : instances_ )
            p->prepare( (std::uint32_t)sampleRate, (std::uint32_t)kChunkFrames );
        preparedRate_ = sampleRate;
        haveLastEnd_  = false;
    }

    if( positional ) {
        // Stateful DSP: a page that does not start exactly where the last one
        // ended is a discontinuity the plugin cannot continue from.
        if( haveLastEnd_ && startPos != lastEnd_ ) {
            TW_LOGD( "plugins", "[slot] non-sequential page at %lld (expected %lld); "
                     "resetting plugin state",
                     (long long)startPos, (long long)lastEnd_ );
            resetInstances_nolock();
        }
        lastEnd_     = startPos + (offset_t)len;
        haveLastEnd_ = true;
    } else {
        // The legacy pull has no page identity, so it cannot claim continuity
        // with anything. Forget where we were rather than pretend.
        haveLastEnd_ = false;
    }

    runChunked_nolock( in, out, len );
}

}  // namespace audio
