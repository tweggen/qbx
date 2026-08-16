#include "tw/plugins/twpluginslotproc.h"

#include "tw/core/twlog.h"
#include "tw/graph/tw303aenv.h"
#include "tw/plugins/twplugininsert.h"

#include <algorithm>
#include <cstring>
#include <limits>

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

// --------------------------------------------- the instrument slot (P3b)

bool twPluginSlotProcessor::isGenerator() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return isGenerator_;
}

void twPluginSlotProcessor::setEventSource( std::shared_ptr<const twEventSource> source )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    if( events_ == source ) return;
    events_ = std::move( source );
    // What the generator produces just changed at every position, so the pages
    // that baked in the old feed have to stop being served. (Swapping the FEED
    // is rare - it happens when the slot is created or the track's chain is
    // rebuilt; a change to what the feed CONTAINS travels the model's own
    // invalidation walk instead.)
    if( isGenerator_ ) {
        haveLastEnd_ = false;
        bumpParamEpoch_nolock();
    }
}

bool twPluginSlotProcessor::hasEventSource() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return events_ != nullptr;
}

void twPluginSlotProcessor::setTempoMap( const twTempoMap &map, bool valid )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    if( tempoValid_ == valid && tempo_ == map ) return;
    tempo_      = map;
    tempoValid_ = valid;
    // The transport is an INPUT to the plugin (a tempo-synced arpeggiator reads
    // it), so a tempo edit can change what a page contains.
    if( isGenerator_ ) bumpParamEpoch_nolock();
}

std::uint32_t twPluginSlotProcessor::tailFrames() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    std::uint32_t t = 0;
    for( const std::unique_ptr<twPlugin> &p : instances_ )
        if( p ) t = std::max( t, p->tailFrames() );
    return t;
}

// An epoch bump does NOT clear this: only a rebuild, a rate change and THIS do
// (design D4). A render whose first page starts exactly where the previous run
// stopped would otherwise continue that run's voices instead of chasing them,
// which is the determinism hole the P3c run barrier closes by calling here.
void twPluginSlotProcessor::forgetContinuity()
{
    std::lock_guard<std::mutex> lock( mutex_ );
    haveLastEnd_ = false;
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
    isGenerator_  = false;
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

    if( nIn == 0 && nOut > 0 ) {
        // THE GENERATOR ROWS (proposal 37 P3b, design 4.3). A 0-input plugin
        // used to fall straight through to Unsupported, which is why an
        // instrument was inaudible rather than merely unsupported.
        //
        // The plugin's audio INPUT is not consumed here - it is the track's own
        // clip material, which the host SUMS onto the generator's output
        // (runGenerator_nolock). That is what keeps an audio clip on an
        // instrument track audible (D3) and what makes "instrument present, no
        // notes" byte-identical to no instrument at all.
        isGenerator_ = true;
        if( nOut == nChannels_ ) {
            mode_ = twPluginSlotMode::DirectGen;
        } else if( nOut == 1 ) {
            // One voice, every channel - centre-panned until a clip carries a
            // pan and the sink is wide (proposal 36 B5).
            mode_ = twPluginSlotMode::MonoSpread;
        } else if( nOut == 2 && nChannels_ == 1 ) {
            mode_ = twPluginSlotMode::GenFold;
        } else if( nOut > nChannels_ ) {
            // Outs 0..C-1 land in the page; the surplus goes into the slot's own
            // buffer, which is where 5.4's aux taps will read it (P9).
            mode_ = twPluginSlotMode::WideGen;
        } else {
            // A generator NARROWER than the page but not mono (0 -> 2 on a
            // 4-channel page): no defined spread, so refuse rather than guess.
            isGenerator_ = false;
        }
        if( isGenerator_ ) {
            instances_.push_back( std::move( first ) );
        }
    }

    if( isGenerator_ ) {
        // The mapping is settled by the generator block above; the effect rows
        // below all assume the plugin CONSUMES its input and would mis-shape a
        // 0-in plugin. (A 0-in plugin whose width has no defined spread left
        // isGenerator_ false and falls through to Unsupported, which is the
        // honest answer and is what proposal 08 always did with it.)
    } else if( nIn == nChannels_ && nOut == nChannels_ ) {
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
        default:
            // Transparent is handled above; the GENERATOR rows never reach this
            // loop at all (render() dispatches them to runGenerator_nolock).
            break;
        }
    }
}

namespace {

// THE FEED SPEAKS MIDI, THE ABI SPEAKS [0,1] (proposal 37 P3b).
//
// tw/events is MODEL data and its `value` is in the MIDI domain, because that
// is the domain the model, the piano roll and the MIDI-OUT pump all read it in
// (SMidiSequence stores `add-note velocity='100'` verbatim; SMidiOutPump sends
// clamp7(e.value) straight onto the wire; the piano roll draws value/127). The
// PLUGIN ABI is normalized - twpluginevents.h, and twNativeInstrument's accent
// threshold is 100/127 - because that is what CLAP and VST3 both use.
//
// This function is the ONE place the two meet, and it is here rather than in
// tw/events because a normalized sequence would force the pump to multiply
// back up and would round-trip a project through a lossy scale.
//
// ProgramChange and ParamValue are deliberately untouched: a program is an
// index and a parameter is already in the plugin's own domain (native for
// CLAP/AU, normalized for VST3 - plugins inv. 26).
void twNormalizeForAbi( twEvent &e )
{
    auto clamp01 = []( double v ) { return v < 0.0 ? 0.0 : ( v > 1.0 ? 1.0 : v ); };
    switch( e.kind ) {
    case twEventKind::NoteOn:
    case twEventKind::NoteOff:
    case twEventKind::NoteChoke:
    case twEventKind::PolyPressure:
    case twEventKind::ControlChange:
    case twEventKind::ChannelPressure:
        e.value = clamp01( e.value / 127.0 );
        break;
    case twEventKind::PitchBend: {
        // The pump's domain: -8192 .. +8191, centre 0. The ABI's is -1 .. +1.
        double b = e.value / 8192.0;
        if( b < -1.0 ) b = -1.0;
        if( b >  1.0 ) b =  1.0;
        e.value = b;
        break;
    }
    default:
        break;
    }
}

}  // namespace

// ------------------------------------------------------- the generator core
//
// PROPOSAL 37 P3b. Everything below runs only for an INSTRUMENT slot (design
// 4.3): the plugin has no audio input, produces its output from EVENTS, and the
// host sums the slot's own audio input onto the result.

void twPluginSlotProcessor::ensureGenScratch_nolock( length_t frames )
{
    if( instances_.empty() ) return;
    const std::size_t nOut = (std::size_t)instances_[0]->ioLayout().audioOutputs;
    // At least two, so GenFold always has its pair.
    const std::size_t want = std::max<std::size_t>( nOut, 2 );
    if( genOut_.size() < want ) genOut_.resize( want );
    for( std::vector<sample_t> &b : genOut_ )
        if( b.size() < (std::size_t)frames ) b.resize( (std::size_t)frames );
    if( genPtrs_.size() < nOut ) genPtrs_.resize( nOut );

    if( outEvents_.size() < twEventLimits::kMaxEventsPerBlock )
        outEvents_.resize( twEventLimits::kMaxEventsPerBlock );
    if( outArena_.size() < twEventLimits::kMaxPayloadBytes )
        outArena_.resize( twEventLimits::kMaxPayloadBytes );
    if( chunkEvents_.capacity() < twEventLimits::kMaxEventsPerBlock )
        chunkEvents_.reserve( twEventLimits::kMaxEventsPerBlock );
    // The per-chunk payload arena is RESERVED, not resized: a sysex or a text
    // event would otherwise allocate on the render path the first time one
    // appeared (CONTRACT invariant 2). clear() keeps the capacity.
    if( chunkArena_.capacity() < twEventLimits::kMaxPayloadBytes )
        chunkArena_.reserve( twEventLimits::kMaxPayloadBytes );
    if( chaseEvents_.capacity() < twEventLimits::kMaxEventsPerBlock )
        chaseEvents_.reserve( twEventLimits::kMaxEventsPerBlock );
}

// The chase set, as the events that put a plugin INTO that state. Controllers
// first and the note-ons last: a bend, a program or a sustain that was already
// in force has to be in force again BEFORE the notes it shaped are re-attacked.
// Sustain is not emitted separately - it IS CC 64 and is already in `cc`.
void twPluginSlotProcessor::chaseToEvents_nolock( const twEventState &chase,
                                                  std::vector<twEvent> &out ) const
{
    auto base = []( twEventKind k, std::int16_t ch ) {
        twEvent e;
        e.time    = 0;
        e.kind    = k;
        e.channel = ch;
        return e;
    };

    // Every branch below ends in twNormalizeForAbi(): the chase set is read out
    // of the same MIDI-domain sequences the window events come from.
    for( const auto &kv : chase.program ) {
        twEvent e = base( twEventKind::ProgramChange, kv.first );
        e.value = (double)kv.second;
        twNormalizeForAbi( e );
        out.push_back( e );
    }
    for( const auto &kv : chase.cc ) {
        twEvent e = base( twEventKind::ControlChange, kv.first.first );
        e.paramId = kv.first.second;
        e.value   = kv.second;
        twNormalizeForAbi( e );
        out.push_back( e );
    }
    for( const auto &kv : chase.bend ) {
        twEvent e = base( twEventKind::PitchBend, kv.first );
        e.value = kv.second;
        twNormalizeForAbi( e );
        out.push_back( e );
    }
    for( const auto &kv : chase.pressure ) {
        twEvent e = base( twEventKind::ChannelPressure, kv.first );
        e.value = kv.second;
        twNormalizeForAbi( e );
        out.push_back( e );
    }
    // The notes LAST. They keep their ids, which is what lets a note-off that
    // arrives later in the very same block release the note the chase attacked.
    for( const twHeldNote &h : chase.notes ) {
        twEvent e = base( twEventKind::NoteOn, h.channel );
        e.key      = h.key;
        e.noteId   = h.noteId;
        e.value    = h.velocity;
        e.duration = 0;   // a chased note always gets its own note-off later
        twNormalizeForAbi( e );
        out.push_back( e );
    }
}

// D4: reset() -> chase stateAt(P-K) at offset 0 -> pre-roll K frames with the
// events at their real offsets, output DISCARDED -> the caller renders the page.
//
//     K = min( max(4096, tailFrames(), P - start(earliest note held at P)), 4 s )
//
// K reaches back to the HELD NOTES, which is the whole point: a pad held since
// 0 s and located at 2 s is pre-rolled from its own note-on, so its envelope
// arrives at P in the state a continuous run would have given it. A note held
// longer than the four-second cap has converged by then, and the cap is what
// bounds the cost. The caller has already reset(); this only rebuilds.
void twPluginSlotProcessor::preRoll_nolock( offset_t pos, int sampleRate )
{
    if( !events_ || pos <= 0 || instances_.empty() ) return;

    const int rate = sampleRate > 0 ? sampleRate : preparedRate_;

    // stateAt() reports each held note's START; a one-frame collect is the
    // cheapest way to ask for it and costs nothing when nothing is held.
    probeBlock_.clear();
    events_->collect( (std::int64_t)pos, 1, probeBlock_ );
    std::int64_t earliest = (std::int64_t)pos;
    for( const twHeldNote &h : probeBlock_.chase.notes )
        if( h.start < earliest ) earliest = h.start;

    std::uint32_t tail = 0;
    for( const std::unique_ptr<twPlugin> &p : instances_ )
        if( p ) tail = std::max( tail, p->tailFrames() );

    std::int64_t k = (std::int64_t)kMinPreRollFrames;
    k = std::max<std::int64_t>( k, (std::int64_t)tail );
    k = std::max<std::int64_t>( k, (std::int64_t)pos - earliest );
    if( rate > 0 )
        k = std::min<std::int64_t>( k, (std::int64_t)rate * kMaxPreRollSeconds );
    k = std::min<std::int64_t>( k, (std::int64_t)pos );   // never before frame 0
    if( k <= 0 ) return;

    const offset_t p0 = pos - (offset_t)k;
    preBlock_.clear();
    events_->collect( (std::int64_t)p0, k, preBlock_ );
    // No input to sum and no output to keep: `out` null IS "discard".
    runGenerator_nolock( nullptr, nullptr, (length_t)k, p0, preBlock_, true, sampleRate );
}

void twPluginSlotProcessor::runGenerator_nolock( const sample_t *const *in,
                                                 sample_t **out, length_t len,
                                                 offset_t startPos,
                                                 const twEventBlock &block,
                                                 bool injectChase, int sampleRate )
{
    if( len <= 0 || instances_.empty() ) return;

    twPlugin   *plug = instances_[0].get();
    const idx_t nOut = (idx_t)plug->ioLayout().audioOutputs;
    const bool  bypassed = bypass_.load( std::memory_order_acquire );
    // INSTRUMENT BYPASS IS SILENCE, NOT A SHORT-CIRCUIT (design 4.3): the
    // plugin still gets every event and its voices still evolve, we simply do
    // not keep the audio. Skipping process() would mean the note-offs inside
    // the bypassed span never arrived, and an un-bypass would resurrect voices
    // that should long since have ended.
    const bool  discard = ( out == nullptr ) || bypassed;

    ensureGenScratch_nolock( kChunkFrames );

    twEventOut sink;
    sink.setStorage( outEvents_.data(), (std::uint32_t)outEvents_.size(),
                     outArena_.data(), (std::uint32_t)outArena_.size() );

    chaseEvents_.clear();
    if( injectChase ) chaseToEvents_nolock( block.chase, chaseEvents_ );

    std::size_t evIdx = 0;

    for( length_t off = 0; off < len; off += kChunkFrames ) {
        const length_t n = std::min<length_t>( kChunkFrames, len - off );
        const bool     lastChunk = ( off + n >= len );

        // ONE SORTED LIST PER CHUNK (plugins inv. 5, amended by P3b): the chase
        // at offset 0 of the first chunk, then the span's own events rebased
        // onto this chunk. The UI's setParam ring is merged by the BACKEND
        // (twClapPlugin::drainEditsIntoEvents and its VST3/AU counterparts),
        // also at offset 0 and ahead of these, so what the plugin sees is one
        // non-decreasing stream and there is no second ring here.
        chunkEvents_.clear();
        chunkArena_.clear();
        if( off == 0 )
            chunkEvents_.insert( chunkEvents_.end(), chaseEvents_.begin(),
                                 chaseEvents_.end() );

        while( evIdx < block.events.size() ) {
            const twEvent &src = block.events[evIdx];
            if( !lastChunk && src.time >= (std::int64_t)( off + n ) ) break;
            ++evIdx;
            if( twEventIsMetadata( src.kind ) ) continue;   // score, not performance

            twEvent      ev = src;
            std::int64_t t  = src.time - (std::int64_t)off;
            if( t < 0 ) t = 0;
            if( t > (std::int64_t)n - 1 ) t = (std::int64_t)n - 1;   // into the block
            ev.time     = t;
            ev.duration = 0;
            if( src.payloadSize ) {
                const std::uint8_t *pl = block.payload( src );
                ev.payloadOffset = (std::uint32_t)chunkArena_.size();
                ev.payloadSize   = pl ? src.payloadSize : 0;
                if( pl ) chunkArena_.insert( chunkArena_.end(), pl, pl + src.payloadSize );
            }
            twNormalizeForAbi( ev );
            if( chunkEvents_.size() < (std::size_t)twEventLimits::kMaxEventsPerBlock )
                chunkEvents_.push_back( ev );
        }

        // Where the plugin writes: the PAGE channels directly wherever that is
        // possible, so the common instrument render copies nothing.
        for( idx_t c = 0; c < nOut; ++c ) {
            sample_t *dst = genOut_[(std::size_t)c].data();
            if( !discard ) {
                if( mode_ == twPluginSlotMode::DirectGen )
                    dst = out[c] + off;
                else if( mode_ == twPluginSlotMode::WideGen && c < nChannels_ )
                    dst = out[c] + off;
            }
            genPtrs_[(std::size_t)c] = dst;
        }

        const twEventList list{
            chunkEvents_.empty() ? nullptr : chunkEvents_.data(),
            (std::uint32_t)chunkEvents_.size(),
            chunkArena_.empty() ? nullptr : chunkArena_.data(),
            (std::uint32_t)chunkArena_.size() };

        twProcessContext ctx;
        ctx.position   = (std::int64_t)( startPos + off );
        // A freeze always represents a moving timeline - an offline render is
        // "playing" as far as a plugin's transport is concerned. What is NOT
        // claimed is a steady sample clock: pages freeze out of order.
        ctx.playing    = true;
        ctx.validFlags = twCtxPosition;
        if( tempoValid_ ) {
            ctx.tempoBpm = tempo_.bpm();
            ctx.tsNum    = tempo_.numerator();
            ctx.tsDen    = tempo_.denominator();
            ctx.validFlags |= twCtxTempo | twCtxTimeSig;
            const double rate = (double)( sampleRate > 0 ? sampleRate : preparedRate_ );
            if( rate > 0.0 ) {
                ctx.ppqPos = ( (double)ctx.position / rate ) * ctx.tempoBpm / 60.0;
                ctx.validFlags |= twCtxPpqPosition;
            }
        }

        sink.clear();
        float *const *const buses[1] = { genPtrs_.data() };
        plug->process( nullptr, buses, (std::uint32_t)n, list, sink, ctx );

        if( !out ) continue;      // pre-roll: the audio is deliberately dropped

        if( discard ) {
            // Bypassed: the generator contributes nothing. The pass-through sum
            // below still runs, so the track's own audio clips stay audible.
            for( idx_t c = 0; c < nChannels_; ++c )
                std::fill( out[c] + off, out[c] + off + n, 0.0f );
        } else {
            switch( mode_ ) {
            case twPluginSlotMode::DirectGen:
            case twPluginSlotMode::WideGen:
                // Written straight into the page. WideGen's surplus sits in
                // genOut_[C..] for 5.4's aux taps (P9).
                break;
            case twPluginSlotMode::MonoSpread: {
                const sample_t *v = genPtrs_[0];
                for( idx_t c = 0; c < nChannels_; ++c )
                    std::copy( v, v + n, out[c] + off );
                break;
            }
            case twPluginSlotMode::GenFold: {
                const sample_t *a = genPtrs_[0];
                const sample_t *b = genPtrs_[1];
                sample_t       *d = out[0] + off;
                for( length_t i = 0; i < n; ++i ) d[i] = 0.5f * ( a[i] + b[i] );
                break;
            }
            default:
                break;
            }
        }

        // THE PASS-THROUGH SUM (design D3). The head insert keeps its audio
        // input plug; the PROCESSOR decides whether the plugin sees it, and a
        // generator does not - the host adds it instead. `x + 0.0f == x`, which
        // is what makes "instrument present, no notes" byte-identical to the
        // render with no instrument at all.
        if( in ) {
            for( idx_t c = 0; c < nChannels_; ++c ) {
                if( !in[c] ) continue;
                sample_t       *d = out[c] + off;
                const sample_t *s = in[c] + off;
                for( length_t i = 0; i < n; ++i ) d[i] += s[i];
            }
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

    // ---------------------------------------------------------- instruments
    //
    // A GENERATOR IS FREEZE-PATH ONLY (design 4.3). The legacy streaming pull
    // is positionless, so it cannot place a single event and has nothing to
    // sound; answering silence and saying so once is the honest report.
    // SMARAGD_REVAL_WORKERS=0 therefore makes an instrument track silent BY
    // DESIGN, and instrument race sweeps never include worker count 0
    // (testkit/CONTRACT.md).
    if( isGenerator_ ) {
        if( !positional ) {
            if( !loggedNoPull_ ) {
                loggedNoPull_ = true;
                TW_LOGW( "plugins", "[slot] an instrument is freeze-path only; the "
                         "legacy pull has no position and renders silence "
                         "(proposal 37 design 4.3)" );
            }
            for( idx_t c = 0; c < nChannels_; ++c )
                if( out[c] ) std::fill( out[c], out[c] + len, 0.0f );
            haveLastEnd_ = false;
            return;
        }

        // CONTINUITY (design D4). A page that does not start exactly where the
        // last one ended is a REPOSITION, not a continuation: reset all notes
        // off, chase what was sounding at P - K, pre-roll K frames with the
        // events at their real offsets and the output discarded. Only then is
        // the page itself rendered - which is why the page's OWN chase set is
        // never re-issued here (it has just been rebuilt into the DSP, and
        // re-attacking it would double every held note).
        if( !haveLastEnd_ || startPos != lastEnd_ ) {
            TW_LOGD( "plugins", "[slot] instrument reposition to %lld (expected %lld); "
                     "reset + chase + pre-roll",
                     (long long)startPos,
                     haveLastEnd_ ? (long long)lastEnd_ : -1LL );
            resetInstances_nolock();
            preRoll_nolock( startPos, sampleRate );
        }
        lastEnd_     = startPos + (offset_t)len;
        haveLastEnd_ = true;

        pageBlock_.clear();
        if( events_ )
            events_->collect( (std::int64_t)startPos, (std::int64_t)len, pageBlock_ );
        runGenerator_nolock( in, out, len, startPos, pageBlock_, false, sampleRate );
        return;
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
