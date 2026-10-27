// twSamplerInstrument — NassauDigital as a `tw` instrument (proposal 56 §4.2,
// nassau-digital DESIGN §9).
//
// A twPlugin AND a twSamplerHost. The voice engine is the NassauDigital core's
// nd::Instrument; this file is the translation between qbx's plugin ABI and
// the core's API, and nothing else:
//
//   * twEvent (chunk-relative, ABI domain [0,1]) -> nd::Event (block offset,
//     MIDI 0..127). Note-on, note-off, choke, and CC 1 as the mod wheel; the
//     rest has no mapping in the core yet and is dropped.
//   * twProcessContext::position -> the core's timeline frame, and the
//     ROUND-ROBIN PAGE (proposal 56 B10): the cell is the qbx page grid,
//     floor(position / twOutputPage::FRAME_CAPACITY), and the core is told a
//     new page has started (nd::Instrument::startPage) whenever the cell
//     changes. A chunk that straddles a cell boundary is rendered as two core
//     blocks, split at the boundary, so a page's choices depend on that page
//     alone whatever the block grid.
//   * twProcessContext::tempoBpm -> nd::Instrument::setTempo, per chunk.
//
// THREADING (R3, proposal 56 invariant 3). Every twSamplerHost setter is
// reached only through twPluginSlotProcessor::applyToInstances(), under the
// processor's mutex — the one render() holds for its whole call — so process()
// reads the program and the samples without a lock of its own, and the core's
// own rule ("a setter never runs concurrently with process()") holds by
// construction. process() allocates nothing: the event buffer is sized in the
// constructor, the voice pool in prepare(). A replaced buffer is parked by the
// core and freed by collectRetired() on the host thread (invariant 5).
//
// DETERMINISM (R8). Reset, the same notes, the same bytes: no clock, no
// random, and the core's control grid counts from each note-on, so the bytes do
// not depend on the 4096-frame chunking either.

#include "tw/sampler/twsamplerhost.h"
#include "tw/sampler/twsamplerregistry.h"

#include "tw/core/twlog.h"
#include "tw/pages/tw_output_page.h"
#include "tw/plugins/twplugin.h"

#include "nd_instrument.h"
#include "nd_version.h"
#include "twsamplerimpl.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <vector>

namespace audio {

const char *const kNassauDigitalUid = "tw.native.nassaudigital";

namespace {

constexpr std::size_t   kStateHeaderSize = 8;
constexpr std::uint8_t  kStateMagic[4]   = { 'T', 'W', 'N', 'D' };
constexpr std::uint16_t kStateVersion    = 1;

// The round-robin cell: the page grid every frozen render is cut on.
constexpr std::int64_t kRrCellFrames = (std::int64_t)twOutputPage::FRAME_CAPACITY;

// Room for a whole block of host events plus one wildcard note-off expanded to
// every key.
constexpr std::size_t kMaxNdEvents = twEventLimits::kMaxEventsPerBlock + 128;

void putU16le( std::uint8_t *p, std::uint16_t v )
{
    p[0] = (std::uint8_t)( v & 0xFF );
    p[1] = (std::uint8_t)( ( v >> 8 ) & 0xFF );
}

std::uint16_t getU16le( const std::uint8_t *p )
{
    return (std::uint16_t)( (std::uint16_t)p[0] | ( (std::uint16_t)p[1] << 8 ) );
}

std::uint8_t toMidi7( double v01 )
{
    long m = std::lround( v01 * 127.0 );
    if( m < 0 ) m = 0;
    if( m > 127 ) m = 127;
    return (std::uint8_t)m;
}

}  // namespace

class twSamplerInstrument final : public twPlugin, public twSamplerHost {
public:
    twSamplerInstrument()
    {
        io_.audioInputs  = 0;   // a generator
        io_.audioOutputs = 2;   // stereo out (DESIGN §9.3, A17); ONE bus until Q8 (B7)
        ndEvents_.resize( kMaxNdEvents );
        std::fill( std::begin( held_ ), std::end( held_ ), false );
    }

    // --- twPlugin -----------------------------------------------------------

    const twPluginIoLayout &ioLayout() const override { return io_; }

    void prepare( std::uint32_t sampleRate, std::uint32_t maxBlock ) override
    {
        (void)maxBlock;   // the core renders any block length into the host's buffers
        if( sampleRate == 0 ) sampleRate = 48000;
        if( !prepared_ || sampleRate != rate_ ) {
            rate_ = sampleRate;
            core_.prepare( (double)rate_ );   // allocates the voice pool; resets
            prepared_ = true;
            recomputeTail();
        }
        resetPageTracking();
    }

    void reset() override
    {
        if( prepared_ ) core_.reset();
        std::fill( std::begin( held_ ), std::end( held_ ), false );
        resetPageTracking();
    }

    // No host parameters in Q0. Which program quantities become automatable
    // parameters is proposal 56 Q-2, open; until it is answered the instrument
    // declares none, and its state blob is the header alone.
    std::size_t       paramCount() const override { return 0; }
    twPluginParamInfo paramInfo( std::size_t ) const override { return twPluginParamInfo{}; }
    double            getParam( std::uint32_t ) const override { return 0.0; }
    void              setParam( std::uint32_t, double ) override {}

    // THE BLOB IS A REFERENCE, NEVER THE PROGRAM (proposal 56 A3, Q0.4): an
    // 8-byte header ('TWND', u16 version, u16 reserved, the CLAP frame's shape,
    // plugins inv. 3) and the program-level parameters once Q-2 names any. The
    // program and every sample live in the model, so a 10 MB sample adds
    // nothing here.
    std::vector<std::uint8_t> saveState() const override
    {
        std::vector<std::uint8_t> blob( kStateHeaderSize, 0 );
        std::memcpy( blob.data(), kStateMagic, sizeof( kStateMagic ) );
        putU16le( blob.data() + 4, kStateVersion );
        putU16le( blob.data() + 6, 0 );
        return blob;
    }

    bool loadState( const std::vector<std::uint8_t> &blob ) override
    {
        if( blob.size() < kStateHeaderSize ) return false;
        if( std::memcmp( blob.data(), kStateMagic, sizeof( kStateMagic ) ) != 0 ) return false;
        const std::uint16_t ver = getU16le( blob.data() + 4 );
        if( ver > kStateVersion ) {
            TW_LOGW( "plugins", "[nassaudigital] state blob is version %u, we understand %u; "
                     "refusing it", (unsigned)ver, (unsigned)kStateVersion );
            return false;
        }
        return true;   // trailing data from a newer minor is ignored (inv. 3)
    }

    twPluginCapabilities capabilities() const override
    {
        twPluginCapabilities c;
        c.acceptsNotes    = true;
        c.emitsNotes      = false;
        c.isInstrument    = true;
        c.supportsNoteIds = false;   // the core matches a note-off by key
        c.notePortsIn     = 1;
        c.notePortsOut    = 0;
        return c;
    }

    std::uint32_t tailFrames() const override
    {
        return tailFrames_.load( std::memory_order_acquire );
    }

    void process( const float *const *in, float *const *out, std::uint32_t nframes ) override
    {
        const twEventList      noEvents{};
        twEventOut             noSink;
        const twProcessContext noCtx{};
        float *const *const    buses[1] = { out };
        process( in, buses, nframes, noEvents, noSink, noCtx );
    }

    void process( const float *const *in, float *const *const *outBuses,
                  std::uint32_t nframes, const twEventList &events,
                  twEventOut &eventsOut, const twProcessContext &ctx ) override
    {
        (void)in;          // a generator: the HOST sums the track's audio on top
        (void)eventsOut;

        float *outL = ( outBuses && outBuses[0] ) ? outBuses[0][0] : nullptr;
        float *outR = ( outBuses && outBuses[0] ) ? outBuses[0][1] : nullptr;
        if( !outL || !outR || nframes == 0 ) return;

        // What this call runs on, published for twSamplerHostStats: plain
        // members set by the setters under the processor's mutex, read here
        // with no lock (R3).
        statProcessCalls_.fetch_add( 1, std::memory_order_relaxed );
        statProgram_.store( programSerial_, std::memory_order_relaxed );
        statMask_.store( routedMask_, std::memory_order_relaxed );
        statSamples_.store( (std::uint32_t)samples_.size(), std::memory_order_relaxed );

        if( !prepared_ ) {
            std::fill( outL, outL + nframes, 0.0f );
            std::fill( outR, outR + nframes, 0.0f );
            return;
        }

        if( ( ctx.validFlags & twCtxTempo ) && ctx.tempoBpm > 0.0 )
            core_.setTempo( ctx.tempoBpm );

        // Translate the chunk's events once, keeping their chunk offsets.
        const std::size_t nEv = translate( events, nframes );

        const bool         havePos = ( ctx.validFlags & twCtxPosition ) != 0;
        const std::int64_t pos     = havePos ? ctx.position : 0;

        // Split at every round-robin cell boundary inside the chunk.
        std::uint32_t done = 0;
        std::size_t   ev   = 0;
        while( done < nframes ) {
            std::uint32_t len = nframes - done;
            if( havePos ) {
                const std::int64_t at   = pos + (std::int64_t)done;
                const std::int64_t cell = floorDiv( at, kRrCellFrames );
                if( !haveCell_ || cell != cell_ ) {
                    core_.startPage();
                    cell_     = cell;
                    haveCell_ = true;
                }
                const std::int64_t toEdge = ( cell + 1 ) * kRrCellFrames - at;
                if( toEdge < (std::int64_t)len ) len = (std::uint32_t)toEdge;
            }
            // This segment's events, rebased onto it.
            std::size_t first = ev;
            while( ev < nEv && (std::uint32_t)ndEvents_[ev].offset < done + len ) {
                ndEvents_[ev].offset -= (std::int32_t)done;
                ++ev;
            }
            core_.process( pos + (std::int64_t)done, nEv > first ? &ndEvents_[first] : nullptr,
                           (int)( ev - first ), outL + done, outR + done, (int)len );
            done += len;
        }

        statVoices_.store( core_.activeVoices(), std::memory_order_relaxed );
    }

    // --- twSamplerHost (reached ONLY through applyToInstances) --------------

    void setProgram( std::shared_ptr<const twSamplerProgram> program ) override
    {
        program_       = std::move( program );
        programSerial_ = program_ ? program_->serial() : 0;
        statInstalled_.store( programSerial_, std::memory_order_relaxed );
        core_.setProgram( program_ ? program_->impl().program : nullptr );
        recomputeTail();
    }

    void setSample( std::uint32_t layerId, std::shared_ptr<const twSamplerSample> sample ) override
    {
        core_.setSample( layerId, sample ? sample->impl().sample : nullptr );
        if( sample ) samples_[layerId] = std::move( sample );
        else         samples_.erase( layerId );
        recomputeTail();
    }

    void setRoutedOutputs( std::uint32_t mask ) override { routedMask_ = mask; }

    int collectRetired() override { return core_.collectGarbage(); }

    twSamplerHostStats stats() const override
    {
        twSamplerHostStats s;
        s.processCalls     = statProcessCalls_.load( std::memory_order_relaxed );
        s.programSerial    = statProgram_.load( std::memory_order_relaxed );
        s.routedMask       = statMask_.load( std::memory_order_relaxed );
        s.samplesInstalled = statSamples_.load( std::memory_order_relaxed );
        s.activeVoices     = statVoices_.load( std::memory_order_relaxed );
        s.installedSerial  = statInstalled_.load( std::memory_order_relaxed );
        return s;
    }

private:
    static std::int64_t floorDiv( std::int64_t a, std::int64_t b )
    {
        std::int64_t q = a / b;
        if( ( a % b ) != 0 && ( ( a < 0 ) != ( b < 0 ) ) ) --q;
        return q;
    }

    void resetPageTracking()
    {
        haveCell_ = false;
        cell_     = 0;
    }

    // twEvent -> nd::Event, in list order. Returns the count. No allocation:
    // ndEvents_ was sized in the constructor.
    std::size_t translate( const twEventList &events, std::uint32_t nframes )
    {
        std::size_t n = 0;
        auto push = [&]( std::int64_t t, nd::Event::Type type, std::uint8_t key,
                         std::uint8_t value ) {
            if( n >= ndEvents_.size() ) return;
            if( t < 0 ) t = 0;
            if( t > (std::int64_t)nframes - 1 ) t = (std::int64_t)nframes - 1;
            nd::Event &e = ndEvents_[n++];
            e.offset = (std::int32_t)t;
            e.type   = type;
            e.key    = key;
            e.value  = value;
        };
        for( std::uint32_t i = 0; i < events.count; ++i ) {
            const twEvent &e = events.events[i];
            switch( e.kind ) {
            case twEventKind::NoteOn: {
                if( e.key < 0 || e.key > 127 ) break;
                std::uint8_t vel = toMidi7( e.value );
                if( vel == 0 ) vel = 1;   // a sounding note-on, however soft
                push( e.time, nd::Event::NoteOn, (std::uint8_t)e.key, vel );
                held_[e.key] = true;
                break;
            }
            case twEventKind::NoteOff:
            case twEventKind::NoteChoke:
                // The core has no choke yet: a choke releases like a note-off.
                if( e.key >= 0 && e.key <= 127 ) {
                    push( e.time, nd::Event::NoteOff, (std::uint8_t)e.key, 0 );
                    held_[e.key] = false;
                } else {
                    // A wildcard off: release every key this instance holds.
                    for( int k = 0; k < 128; ++k ) {
                        if( !held_[k] ) continue;
                        push( e.time, nd::Event::NoteOff, (std::uint8_t)k, 0 );
                        held_[k] = false;
                    }
                }
                break;
            case twEventKind::ControlChange:
                if( e.paramId == 1 )   // CC 1, the mod wheel (DESIGN §4.4)
                    push( e.time, nd::Event::ModWheel, 0, toMidi7( e.value ) );
                break;
            default:
                break;   // no mapping in the core yet
            }
        }
        return n;
    }

    // How long a voice can outlast its note-off, in frames: the longest amp
    // release in the program, plus — for a one-shot zone, which plays its sample
    // to the end whatever the note-off says — the longest installed sample.
    // Host thread (the setters and prepare()); read by the processor for the
    // pre-roll and the project end.
    void recomputeTail()
    {
        double seconds = 0.0;
        bool   oneShot = false;
        if( program_ && program_->impl().program ) {
            for( const nd::Zone &z : program_->impl().program->zones ) {
                seconds = std::max( seconds, z.release );
                if( z.mode == nd::PlayMode::OneShot || z.mode == nd::PlayMode::TempoOneShot )
                    oneShot = true;
            }
        }
        if( oneShot ) {
            for( const auto &kv : samples_ ) {
                const twSamplerSample &s = *kv.second;
                if( s.sampleRate() > 0.0 )
                    seconds = std::max( seconds, (double)s.frames() / s.sampleRate() );
            }
        }
        const double frames = seconds * (double)rate_;
        tailFrames_.store( frames >= 4294967295.0 ? 0xFFFFFFFFu : (std::uint32_t)frames,
                           std::memory_order_release );
    }

    twPluginIoLayout io_{};
    nd::Instrument   core_;
    std::uint32_t    rate_     = 48000;
    bool             prepared_ = false;

    // Installed snapshots (set under the processor's mutex, read by process()).
    std::shared_ptr<const twSamplerProgram>                          program_;
    std::uint64_t                                                    programSerial_ = 0;
    std::map<std::uint32_t, std::shared_ptr<const twSamplerSample> > samples_;
    std::uint32_t                                                    routedMask_ = 0;

    // Render state.
    std::vector<nd::Event> ndEvents_;
    bool                   held_[128];
    bool                   haveCell_ = false;
    std::int64_t           cell_     = 0;

    std::atomic<std::uint32_t> tailFrames_{ 0 };

    // twSamplerHostStats, written by process().
    std::atomic<std::uint64_t> statProcessCalls_{ 0 };
    std::atomic<std::uint64_t> statProgram_{ 0 };
    std::atomic<std::uint32_t> statMask_{ 0 };
    std::atomic<std::uint32_t> statSamples_{ 0 };
    std::atomic<int>           statVoices_{ 0 };
    std::atomic<std::uint64_t> statInstalled_{ 0 };   // written by setProgram()
};

// ----------------------------------------------------------- registration ---

twPluginDescriptor nassauDigitalDescriptor()
{
    twPluginDescriptor d;
    d.format         = "tw";
    d.uid            = kNassauDigitalUid;
    d.path           = "";   // linked in, never scanned
    d.name           = "NassauDigital";
    d.vendor         = "Nassau";
    d.io             = { 0, 2 };
    d.isInstrument   = true;
    d.acceptsNotes   = true;
    d.emitsNotes     = false;
    d.eventPortsIn   = 1;
    d.eventPortsOut  = 0;
    d.nOutBuses      = 1;          // until Q8 (proposal 56 B7, invariant 10)
    d.outBusChannels = { 2 };
    return d;
}

void registerNassauDigital( twPluginRegistry &registry )
{
    registry.registerBuiltin( nassauDigitalDescriptor(), []() -> std::unique_ptr<twPlugin> {
        return std::unique_ptr<twPlugin>( new twSamplerInstrument );
    } );
    TW_LOGI( "plugins", "[nassaudigital] core %s registered as %s", nd::coreVersion(),
             kNassauDigitalUid );
}

}  // namespace audio
