// AudioUnit backend: audio::twPlugin over an AudioUnit (proposal 08 M8).
//
// Compiled only on macOS with TW_HAVE_AU. Plain C AudioToolbox / CoreFoundation —
// no Objective-C, so this is .cc.
//
// Threading. process()/reset() run on the schedule worker pool (never the RT
// callback — twRtThreadGuard enforces that upstream). Activation transitions
// (prepare/uninitialize) take hostMutex_; process() only reads the atomic flags,
// exactly like the CLAP backend. AudioUnitGetParameter/SetParameter are
// documented realtime- and thread-safe, so — unlike CLAP — there is no lock-free
// parameter ring: setParam() calls the unit directly.
//
// The render model. An effect PULLS its input through a render callback. We
// install one on the input scope that memcpies from the caller's de-interleaved
// `in` buffers (the ones twPluginInsert owns), and point the output
// AudioBufferList at the caller's `out` buffers, then call AudioUnitRender.

#include "twaumodule.h"

#include "tw/core/twlog.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace audio {

namespace {

// Our own frame around the AU's opaque ClassInfo blob. Same 8-byte shape as the
// CLAP backend (CONTRACT invariant 3), but a distinct magic so a blob from one
// format is rejected rather than misread if it ever reaches the other backend.
constexpr std::size_t   kStateHeaderSize = 8;
constexpr std::uint8_t  kStateMagic[4]   = { 'T', 'W', 'A', 'U' };
constexpr std::uint16_t kStateVersion    = 1;

void putU16le( std::uint8_t *p, std::uint16_t v )
{
    p[0] = (std::uint8_t)( v & 0xFF );
    p[1] = (std::uint8_t)( ( v >> 8 ) & 0xFF );
}

std::uint16_t getU16le( const std::uint8_t *p )
{
    return (std::uint16_t)( (std::uint16_t) p[0] | ( (std::uint16_t) p[1] << 8 ) );
}

void fillFloatASBD( AudioStreamBasicDescription &a, double sampleRate, UInt32 channels )
{
    std::memset( &a, 0, sizeof( a ) );
    a.mSampleRate       = sampleRate;
    a.mFormatID         = kAudioFormatLinearPCM;
    a.mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked
                          | kAudioFormatFlagIsNonInterleaved;
    a.mBitsPerChannel   = 32;
    a.mChannelsPerFrame = channels;
    a.mFramesPerPacket  = 1;
    a.mBytesPerFrame    = sizeof( float );   // per-channel, non-interleaved
    a.mBytesPerPacket   = sizeof( float );
}

}  // namespace

class twAuPlugin final : public twPlugin {
public:
    static std::unique_ptr<twAuPlugin> create( const std::string &uid );
    ~twAuPlugin() override;

    const twPluginIoLayout &ioLayout() const override { return io_; }

    void prepare( std::uint32_t sampleRate, std::uint32_t maxBlock ) override;
    void process( const float *const *in, float *const *out,
                  std::uint32_t nframes ) override;
    void process( const float *const *in, float *const *const *outBuses,
                  std::uint32_t nframes, const twEventList &events,
                  twEventOut &eventsOut, const twProcessContext &ctx ) override;
    void reset() override;

    std::size_t       paramCount() const override { return params_.size(); }
    twPluginParamInfo paramInfo( std::size_t i ) const override;
    double            getParam( std::uint32_t id ) const override;
    void              setParam( std::uint32_t id, double v ) override;
    std::string       paramValueText( std::uint32_t id, double v ) const override;

    std::vector<std::uint8_t> saveState() const override;
    bool loadState( const std::vector<std::uint8_t> & ) override;

    std::uint32_t reportedLatency() const override
    {
        return latencyFrames_.load( std::memory_order_acquire );
    }
    bool supportsNativeEditor() const override { return hasGui_; }

    twPluginCapabilities capabilities() const override { return caps_; }
    std::size_t          audioOutBusCount() const override { return outElemChans_.size(); }
    twPluginBusInfo      audioOutBus( std::size_t i ) const override;
    std::uint32_t        tailFrames() const override;

private:
    twAuPlugin() = default;

    bool init( const std::string &uid );
    void uninitialize();                 // hostMutex_ must be held
    void readIoLayout();
    void readOutputElements();
    void readCapabilities();
    void readParams();
    void readGui();

    // Post MIDI 1.0 bytes and scheduled parameter changes to the unit BEFORE
    // AudioUnitRender (proposal 36 §5.2) — AU has no event list in the render
    // call, so an event is delivered by calling the unit ahead of it with the
    // offset it should take effect at.
    void postEvents( const twEventList &events, std::uint32_t nframes );

    // Fallback for AUs that implement no ParameterStringFromValue: synthesize
    // "<number><unit>" from the parameter's reported AudioUnitParameterUnit.
    // Empty for indexed/boolean/unknown units, so the host's numeric formatter runs.
    std::string unitSuffixText( std::uint32_t id, double v ) const;

    static OSStatus renderInputCb( void *refCon, AudioUnitRenderActionFlags *flags,
                                   const AudioTimeStamp *ts, UInt32 bus,
                                   UInt32 nframes, AudioBufferList *io );

    AudioComponentInstance unit_ = nullptr;

    twPluginIoLayout io_{};
    std::string      uid_;
    bool             hasGui_    = false;
    double           latencySec_ = 0.0;

    twPluginCapabilities caps_{};
    // Channel count per OUTPUT ELEMENT. Element 0 is the main out; 1..N are the
    // aux outs a multi-output instrument declares (proposal 36 §5.4). Only
    // element 0 is rendered today — an aux element needs its OWN
    // AudioUnitRender at the same timestamp, which is P9's job.
    std::vector<std::uint16_t> outElemChans_;
    // The AU component type, so process() knows whether MusicDeviceMIDIEvent is
    // even callable on this unit.
    std::uint32_t componentType_ = 0;

    std::vector<twPluginParamInfo>       params_;
    std::vector<AudioUnitParameterID>    paramIds_;
    // Native unit per parameter (index-aligned with paramIds_). readParams()
    // collapses this to isStepped for the ABI; the full value drives the
    // unitSuffixText() display fallback.
    std::vector<AudioUnitParameterUnit>  paramUnits_;

    // Output AudioBufferList storage (variable-length struct), sized in prepare()
    // so process() never allocates. Filled with the caller's out[] pointers.
    std::vector<std::uint8_t> outAblStorage_;

    // Set at the top of process(), read synchronously by the render callback on
    // the same thread (the slot processor serializes per instance).
    const float *const *curIn_      = nullptr;
    std::uint32_t       curInChans_ = 0;
    Float64             renderTime_ = 0.0;

    mutable std::mutex         hostMutex_;
    std::atomic<bool>          active_{ false };
    std::atomic<bool>          processFailed_{ false };
    std::atomic<std::uint32_t> preparedMax_{ 0 };
    std::atomic<std::uint32_t> latencyFrames_{ 0 };
    std::uint32_t              preparedRate_ = 0;   // hostMutex_ only
};

// --- construction -----------------------------------------------------------

std::unique_ptr<twAuPlugin> twAuPlugin::create( const std::string &uid )
{
    std::unique_ptr<twAuPlugin> p( new twAuPlugin() );
    if( !p->init( uid ) )
        return nullptr;
    return p;
}

bool twAuPlugin::init( const std::string &uid )
{
    uid_ = uid;

    std::uint32_t t = 0, s = 0, mfr = 0;
    if( !auCodesFromUid( uid, t, s, mfr ) ) {
        TW_LOGE( "plugins", "[au] '%s' is not a valid AU uid", uid.c_str() );
        return false;
    }

    AudioComponentDescription want;
    want.componentType         = t;
    want.componentSubType      = s;
    want.componentManufacturer = mfr;
    want.componentFlags        = 0;
    want.componentFlagsMask    = 0;

    AudioComponent comp = AudioComponentFindNext( nullptr, &want );
    if( !comp ) {
        TW_LOGE( "plugins", "[au] no registered component for uid '%s'", uid.c_str() );
        return false;
    }

    const OSStatus st = AudioComponentInstanceNew( comp, &unit_ );
    if( st != noErr || !unit_ ) {
        TW_LOGE( "plugins", "[au] AudioComponentInstanceNew failed for '%s' (err %d)",
                 uid.c_str(), (int) st );
        unit_ = nullptr;
        return false;
    }

    componentType_ = t;
    readIoLayout();
    readOutputElements();
    readCapabilities();
    readParams();
    readGui();

    TW_LOGI( "plugins", "[au] instantiated '%s' (%u in / %u out, %u param(s))",
             uid.c_str(), (unsigned) io_.audioInputs, (unsigned) io_.audioOutputs,
             (unsigned) params_.size() );
    return true;
}

twAuPlugin::~twAuPlugin()
{
    {
        std::lock_guard<std::mutex> lock( hostMutex_ );
        uninitialize();
    }
    if( unit_ )
        AudioComponentInstanceDispose( unit_ );
    unit_ = nullptr;
}

void twAuPlugin::readIoLayout()
{
    // The unit's default stream format tells us its natural channel count; most
    // effects report stereo. A 0 there means "unspecified" — treat as stereo, the
    // insert's channel-mismatch policy handles the rest.
    auto chansOf = [&]( AudioUnitScope scope ) -> std::uint16_t {
        AudioStreamBasicDescription a;
        UInt32                      sz = sizeof( a );
        std::memset( &a, 0, sizeof( a ) );
        if( AudioUnitGetProperty( unit_, kAudioUnitProperty_StreamFormat, scope, 0,
                                  &a, &sz ) == noErr && a.mChannelsPerFrame > 0 )
            return (std::uint16_t) a.mChannelsPerFrame;
        return 2;
    };
    io_.audioInputs  = chansOf( kAudioUnitScope_Input );
    io_.audioOutputs = chansOf( kAudioUnitScope_Output );
}

void twAuPlugin::readParams()
{
    UInt32 sz = 0;
    if( AudioUnitGetPropertyInfo( unit_, kAudioUnitProperty_ParameterList,
                                  kAudioUnitScope_Global, 0, &sz, nullptr ) != noErr
        || sz == 0 )
        return;

    std::vector<AudioUnitParameterID> ids( sz / sizeof( AudioUnitParameterID ) );
    if( AudioUnitGetProperty( unit_, kAudioUnitProperty_ParameterList,
                              kAudioUnitScope_Global, 0, ids.data(), &sz ) != noErr )
        return;

    params_.reserve( ids.size() );
    paramIds_.reserve( ids.size() );
    for( AudioUnitParameterID id : ids ) {
        AudioUnitParameterInfo info;
        UInt32                 isz = sizeof( info );
        std::memset( &info, 0, sizeof( info ) );
        // Parameter metadata is a property whose ELEMENT is the parameter id.
        if( AudioUnitGetProperty( unit_, kAudioUnitProperty_ParameterInfo,
                                  kAudioUnitScope_Global, id, &info, &isz ) != noErr )
            continue;

        twPluginParamInfo pi;
        pi.id           = (std::uint32_t) id;
        pi.minValue     = info.minValue;
        pi.maxValue     = info.maxValue;
        pi.defaultValue = info.defaultValue;
        pi.isStepped    = ( info.unit == kAudioUnitParameterUnit_Indexed )
                          || ( info.unit == kAudioUnitParameterUnit_Boolean );

        if( ( info.flags & kAudioUnitParameterFlag_HasCFNameString ) && info.cfNameString ) {
            const CFIndex len = CFStringGetLength( info.cfNameString );
            std::string   nm( (std::size_t) len * 4 + 1, '\0' );
            if( CFStringGetCString( info.cfNameString, &nm[0], (CFIndex) nm.size(),
                                    kCFStringEncodingUTF8 ) ) {
                nm.resize( std::char_traits<char>::length( nm.c_str() ) );
                pi.name = nm;
            }
            if( info.flags & kAudioUnitParameterFlag_CFNameRelease )
                CFRelease( info.cfNameString );
        }
        if( pi.name.empty() )
            pi.name = info.name;   // fixed-size C string fallback

        params_.push_back( std::move( pi ) );
        paramIds_.push_back( id );
        paramUnits_.push_back( info.unit );
    }
}

// The OUTPUT ELEMENTS (proposal 36 §5.2/§5.4). AU spells a multi-output plugin
// as several output elements on the output scope, each rendered by its OWN
// AudioUnitRender at the same timestamp. Only element 0 is rendered today;
// enumerating them is what lets the descriptor report nOutBuses honestly so P9
// can route the rest to return tracks.
void twAuPlugin::readOutputElements()
{
    outElemChans_.clear();
    if( !unit_ )
        return;

    UInt32 count = 0;
    UInt32 size  = sizeof( count );
    if( AudioUnitGetProperty( unit_, kAudioUnitProperty_ElementCount,
                              kAudioUnitScope_Output, 0, &count, &size ) != noErr )
        count = io_.audioOutputs > 0 ? 1 : 0;

    for( UInt32 e = 0; e < count; ++e ) {
        AudioStreamBasicDescription asbd;
        std::memset( &asbd, 0, sizeof( asbd ) );
        UInt32 asbdSize = sizeof( asbd );
        if( AudioUnitGetProperty( unit_, kAudioUnitProperty_StreamFormat,
                                  kAudioUnitScope_Output, e, &asbd, &asbdSize ) != noErr )
            continue;
        outElemChans_.push_back( (std::uint16_t) asbd.mChannelsPerFrame );
    }
    if( outElemChans_.empty() && io_.audioOutputs > 0 )
        outElemChans_.push_back( io_.audioOutputs );
}

// AU has no capability query: what a unit can do with events follows from its
// COMPONENT TYPE. A MusicDevice (aumu) or MusicEffect (aumf) accepts MIDI; a
// plain Effect (aufx) does not. A MIDI processor (aumi) both accepts and emits.
void twAuPlugin::readCapabilities()
{
    caps_ = twPluginCapabilities{};

    const bool isMusicDevice = componentType_ == (std::uint32_t) kAudioUnitType_MusicDevice;
    const bool isMusicEffect = componentType_ == (std::uint32_t) kAudioUnitType_MusicEffect;
    const bool isMidiProc    = componentType_ == (std::uint32_t) kAudioUnitType_MIDIProcessor;

    caps_.acceptsNotes  = isMusicDevice || isMusicEffect || isMidiProc;
    caps_.isInstrument  = isMusicDevice;
    caps_.notePortsIn   = caps_.acceptsNotes ? 1 : 0;

    // MIDI OUT is opt-in: a unit that offers kAudioUnitProperty_MIDIOutputCallbackInfo
    // can emit. The callback itself must be installed BEFORE AudioUnitInitialize,
    // which is a lifecycle change this phase does not make — so the capability is
    // reported and the lane is not yet wired (recorded in CONTRACT known debt).
    UInt32 size = 0;
    Boolean writable = false;
    if( unit_ && AudioUnitGetPropertyInfo( unit_, kAudioUnitProperty_MIDIOutputCallbackInfo,
                                           kAudioUnitScope_Global, 0, &size, &writable )
            == noErr && size > 0 ) {
        caps_.emitsNotes   = true;
        caps_.notePortsOut = 1;
    }

    // AU MIDI 1.0 has no note ids and no per-note expression (MIDI 2.0 via
    // MusicDeviceMIDIEventList would, and is a later phase).
    caps_.wantsMidi1Raw          = caps_.acceptsNotes;
    caps_.supportsNoteIds        = false;
    caps_.supportsNoteExpression = false;
    caps_.emitsParamChanges      = false;
}

twPluginBusInfo twAuPlugin::audioOutBus( std::size_t i ) const
{
    twPluginBusInfo b;
    if( i >= outElemChans_.size() )
        return b;
    b.channels = outElemChans_[i];
    b.isMain   = ( i == 0 );
    return b;
}

std::uint32_t twAuPlugin::tailFrames() const
{
    if( !unit_ )
        return 0;
    Float64 tailSec = 0.0;
    UInt32  size    = sizeof( tailSec );
    if( AudioUnitGetProperty( unit_, kAudioUnitProperty_TailTime,
                              kAudioUnitScope_Global, 0, &tailSec, &size ) != noErr )
        return 0;
    if( tailSec <= 0.0 )
        return 0;
    const double rate = (double) preparedMax_.load( std::memory_order_acquire ) > 0.0
                            ? (double) preparedRate_ : 48000.0;
    double frames = tailSec * ( rate > 0.0 ? rate : 48000.0 );
    if( frames > 480000.0 )
        frames = 480000.0;   // 10 s cap, as the other backends
    return (std::uint32_t) frames;
}

void twAuPlugin::readGui()
{
    UInt32   sz  = 0;
    Boolean  wr  = false;
    hasGui_ = ( AudioUnitGetPropertyInfo( unit_, kAudioUnitProperty_CocoaUI,
                                          kAudioUnitScope_Global, 0, &sz, &wr ) == noErr
                && sz > 0 );
}

// --- lifecycle --------------------------------------------------------------

void twAuPlugin::prepare( std::uint32_t sampleRate, std::uint32_t maxBlock )
{
    if( !unit_ )
        return;
    if( maxBlock == 0 )
        maxBlock = 1;

    std::lock_guard<std::mutex> lock( hostMutex_ );

    if( active_.load( std::memory_order_relaxed ) && preparedRate_ == sampleRate
        && preparedMax_.load( std::memory_order_relaxed ) == maxBlock )
        return;   // idempotent steady state

    uninitialize();

    AudioStreamBasicDescription inFmt, outFmt;
    fillFloatASBD( inFmt, (double) sampleRate, io_.audioInputs );
    fillFloatASBD( outFmt, (double) sampleRate, io_.audioOutputs );

    auto fail = [&]( const char *what, OSStatus st ) {
        TW_LOGE( "plugins", "[au] %s failed for '%s' (err %d); the insert will pass "
                 "audio through unchanged", what, uid_.c_str(), (int) st );
        processFailed_.store( true, std::memory_order_release );
    };

    OSStatus st;
    if( io_.audioInputs > 0 ) {
        st = AudioUnitSetProperty( unit_, kAudioUnitProperty_StreamFormat,
                                   kAudioUnitScope_Input, 0, &inFmt, sizeof( inFmt ) );
        if( st != noErr ) { fail( "set input StreamFormat", st ); return; }
    }
    st = AudioUnitSetProperty( unit_, kAudioUnitProperty_StreamFormat,
                               kAudioUnitScope_Output, 0, &outFmt, sizeof( outFmt ) );
    if( st != noErr ) { fail( "set output StreamFormat", st ); return; }

    UInt32 maxF = maxBlock;
    st = AudioUnitSetProperty( unit_, kAudioUnitProperty_MaximumFramesPerSlice,
                               kAudioUnitScope_Global, 0, &maxF, sizeof( maxF ) );
    if( st != noErr ) { fail( "set MaximumFramesPerSlice", st ); return; }

    AURenderCallbackStruct cb;
    cb.inputProc       = &twAuPlugin::renderInputCb;
    cb.inputProcRefCon = this;
    st = AudioUnitSetProperty( unit_, kAudioUnitProperty_SetRenderCallback,
                               kAudioUnitScope_Input, 0, &cb, sizeof( cb ) );
    if( st != noErr ) { fail( "set input render callback", st ); return; }

    st = AudioUnitInitialize( unit_ );
    if( st != noErr ) { fail( "AudioUnitInitialize", st ); return; }

    // Output ABL storage: one AudioBuffer per output channel, filled per call.
    const std::size_t nOut = io_.audioOutputs;
    outAblStorage_.assign(
        sizeof( AudioBufferList ) + ( nOut > 0 ? nOut - 1 : 0 ) * sizeof( AudioBuffer ),
        0 );

    // Latency is meaningful only once initialized.
    Float64 lat = 0.0;
    UInt32  lsz = sizeof( lat );
    if( AudioUnitGetProperty( unit_, kAudioUnitProperty_Latency, kAudioUnitScope_Global,
                              0, &lat, &lsz ) == noErr )
        latencySec_ = lat;
    latencyFrames_.store(
        (std::uint32_t) std::lround( latencySec_ * (double) sampleRate ),
        std::memory_order_release );

    preparedRate_ = sampleRate;
    preparedMax_.store( maxBlock, std::memory_order_release );
    processFailed_.store( false, std::memory_order_release );
    renderTime_ = 0.0;

    // Published last (release): process() reads active_ without the mutex, so all
    // the state it will touch must already be visible.
    active_.store( true, std::memory_order_release );
}

// hostMutex_ must be held.
void twAuPlugin::uninitialize()
{
    if( unit_ && active_.exchange( false, std::memory_order_acq_rel ) )
        AudioUnitUninitialize( unit_ );
}

void twAuPlugin::reset()
{
    if( unit_ && active_.load( std::memory_order_acquire ) )
        AudioUnitReset( unit_, kAudioUnitScope_Global, 0 );
}

// --- render callback (pulls input) ------------------------------------------

OSStatus twAuPlugin::renderInputCb( void *refCon, AudioUnitRenderActionFlags *,
                                    const AudioTimeStamp *, UInt32, UInt32 nframes,
                                    AudioBufferList *io )
{
    twAuPlugin *self = (twAuPlugin *) refCon;
    if( !io )
        return noErr;
    const float *const *in = self->curIn_;
    for( UInt32 c = 0; c < io->mNumberBuffers; ++c ) {
        float       *dst   = (float *) io->mBuffers[c].mData;
        const UInt32 bytes = io->mBuffers[c].mDataByteSize;
        if( !dst )
            continue;
        UInt32 frames = bytes / (UInt32) sizeof( float );
        if( frames > nframes )
            frames = nframes;
        if( in && c < self->curInChans_ && in[c] )
            std::memcpy( dst, in[c], (std::size_t) frames * sizeof( float ) );
        else
            std::memset( dst, 0, bytes );
    }
    return noErr;
}

// --- processing -------------------------------------------------------------

// Post the block's events to the unit BEFORE AudioUnitRender (proposal 36 §5.2).
//
// AU has no event list in the render call at all: MusicDeviceMIDIEvent takes an
// inOffsetSampleFrame and must be called ahead of the render for the block the
// offset belongs to. Parameter changes take the same shape through
// AudioUnitScheduleParameters, which is the ONLY way to get a sample-accurate
// parameter step out of an AU.
//
// NOT VERIFIED ON HARDWARE. This phase was implemented on Windows, where the
// whole file is #ifdef'd out; it is written to the documented API and reviewed
// against it, but no AU has ever seen it. Recorded in plugins/CONTRACT.md and in
// plan/STATE.md rather than presented as gated.
void twAuPlugin::postEvents( const twEventList &list, std::uint32_t nframes )
{
    if( !unit_ || list.count == 0 )
        return;

    std::vector<AudioUnitParameterEvent> paramEvents;

    for( std::uint32_t i = 0; i < list.count; ++i ) {
        const twEvent &ev = list.events[i];
        if( twEventIsMetadata( ev.kind ) )
            continue;

        UInt32 offset = ev.time < 0 ? 0u : (UInt32) ev.time;
        if( nframes > 0 && offset >= nframes )
            offset = nframes - 1;

        const UInt32 chan = (UInt32) ( ev.channel >= 0 ? ( ev.channel & 0x0F ) : 0 );
        const UInt32 key  = (UInt32) ( ev.key >= 0 ? ev.key : 60 );

        auto midi = [&]( UInt32 status, UInt32 d1, UInt32 d2 ) {
            if( !caps_.acceptsNotes )
                return;
            MusicDeviceMIDIEvent( unit_, status | chan, d1, d2, offset );
        };

        switch( ev.kind ) {
        case twEventKind::NoteOn:
            midi( 0x90, key, (UInt32) ( ev.value * 127.0 + 0.5 ) );
            break;
        case twEventKind::NoteOff:
            midi( 0x80, key, (UInt32) ( ev.value * 127.0 + 0.5 ) );
            break;
        case twEventKind::PolyPressure:
            midi( 0xA0, key, (UInt32) ( ev.value * 127.0 + 0.5 ) );
            break;
        case twEventKind::ControlChange:
            midi( 0xB0, (UInt32) ( ev.paramId & 0x7F ),
                  (UInt32) ( ev.value * 127.0 + 0.5 ) );
            break;
        case twEventKind::ProgramChange:
            midi( 0xC0, (UInt32) ev.value & 0x7F, 0 );
            break;
        case twEventKind::ChannelPressure:
            midi( 0xD0, (UInt32) ( ev.value * 127.0 + 0.5 ), 0 );
            break;
        case twEventKind::PitchBend: {
            double b = ev.value;
            if( b < -1.0 ) b = -1.0;
            if( b > 1.0 ) b = 1.0;
            const int raw = (int) ( 8192.0 + b * 8191.0 + 0.5 );
            midi( 0xE0, (UInt32) ( raw & 0x7F ), (UInt32) ( ( raw >> 7 ) & 0x7F ) );
            break;
        }
        case twEventKind::Sysex: {
            const std::uint8_t *bytes = list.payloadOf( ev );
            if( bytes && caps_.acceptsNotes )
                MusicDeviceSysEx( unit_, bytes, (UInt32) ev.payloadSize );
            break;
        }
        case twEventKind::ParamValue: {
            AudioUnitParameterEvent pe;
            std::memset( &pe, 0, sizeof( pe ) );
            pe.scope        = kAudioUnitScope_Global;
            pe.element      = 0;
            pe.parameter    = (AudioUnitParameterID) ev.paramId;
            pe.eventType    = kParameterEvent_Immediate;
            pe.eventValues.immediate.bufferOffset = offset;
            pe.eventValues.immediate.value        = (AudioUnitParameterValue) ev.value;
            paramEvents.push_back( pe );
            break;
        }
        default:
            break;   // note ids, choke, expression: no AU MIDI 1.0 equivalent
        }
    }

    if( !paramEvents.empty() )
        AudioUnitScheduleParameters( unit_, paramEvents.data(),
                                     (UInt32) paramEvents.size() );
}

// The LEGACY overload — an empty list, an unreachable sink, an invalid context,
// so it runs exactly the pre-36 instructions.
void twAuPlugin::process( const float *const *in, float *const *out,
                          std::uint32_t nframes )
{
    const twEventList      noEvents{};
    twEventOut             noSink;
    const twProcessContext noCtx{};
    float *const *const    outBuses[1] = { out };
    process( in, outBuses, nframes, noEvents, noSink, noCtx );
}

void twAuPlugin::process( const float *const *in, float *const *const *outBuses,
                          std::uint32_t nframes, const twEventList &hostEvents,
                          twEventOut &eventsOut, const twProcessContext &ctx )
{
    (void) eventsOut;   // AU MIDI-out needs a callback installed before init
    (void) ctx;         // kAudioUnitProperty_HostCallbacks is a later phase

    // Only element 0 is rendered; the aux elements need their own render call
    // (proposal 36 §5.4, P9).
    float *const *out = ( outBuses && !outElemChans_.empty() ) ? outBuses[0] : nullptr;

    const std::uint32_t nIn  = io_.audioInputs;
    const std::uint32_t nOut = io_.audioOutputs;

    auto passThrough = [&]() {
        if( !out )
            return;
        for( std::uint32_t c = 0; c < nOut; ++c ) {
            if( !out[c] )
                continue;
            if( c < nIn && in && in[c] )
                std::memcpy( out[c], in[c], (std::size_t) nframes * sizeof( float ) );
            else
                std::memset( out[c], 0, (std::size_t) nframes * sizeof( float ) );
        }
    };

    if( !unit_ || nframes == 0 || processFailed_.load( std::memory_order_acquire )
        || !active_.load( std::memory_order_acquire ) ) {
        passThrough();
        return;
    }
    const std::uint32_t maxBlock = preparedMax_.load( std::memory_order_acquire );
    if( nframes > maxBlock ) {
        TW_LOGE( "plugins", "[au] process(%u) exceeds prepared max %u for '%s'; "
                 "passing audio through", (unsigned) nframes, (unsigned) maxBlock,
                 uid_.c_str() );
        passThrough();
        return;
    }

    // Events go in BEFORE the render, with their own sample offsets.
    postEvents( hostEvents, nframes );

    curIn_      = in;
    curInChans_ = nIn;

    AudioBufferList *abl = (AudioBufferList *) outAblStorage_.data();
    abl->mNumberBuffers  = nOut;
    for( std::uint32_t c = 0; c < nOut; ++c ) {
        abl->mBuffers[c].mNumberChannels = 1;
        abl->mBuffers[c].mDataByteSize   = nframes * (UInt32) sizeof( float );
        abl->mBuffers[c].mData           = out ? out[c] : nullptr;
    }

    AudioTimeStamp ts;
    std::memset( &ts, 0, sizeof( ts ) );
    ts.mSampleTime = renderTime_;
    ts.mFlags      = kAudioTimeStampSampleTimeValid;

    AudioUnitRenderActionFlags flags = 0;
    const OSStatus st = AudioUnitRender( unit_, &flags, &ts, 0, nframes, abl );
    renderTime_ += nframes;

    if( st != noErr ) {
        TW_LOGE( "plugins", "[au] AudioUnitRender failed for '%s' (err %d); disabling "
                 "processing for this instance", uid_.c_str(), (int) st );
        processFailed_.store( true, std::memory_order_release );
        passThrough();
    }
}

// --- parameters -------------------------------------------------------------

twPluginParamInfo twAuPlugin::paramInfo( std::size_t i ) const
{
    if( i < params_.size() )
        return params_[i];
    return twPluginParamInfo{};
}

double twAuPlugin::getParam( std::uint32_t id ) const
{
    if( !unit_ )
        return 0.0;
    AudioUnitParameterValue v = 0.0f;
    if( AudioUnitGetParameter( unit_, (AudioUnitParameterID) id,
                               kAudioUnitScope_Global, 0, &v ) == noErr )
        return (double) v;
    return 0.0;
}

void twAuPlugin::setParam( std::uint32_t id, double v )
{
    if( !unit_ )
        return;
    for( std::size_t i = 0; i < params_.size(); ++i ) {
        if( params_[i].id == id ) {
            if( v < params_[i].minValue ) v = params_[i].minValue;
            if( v > params_[i].maxValue ) v = params_[i].maxValue;
            // AudioUnitSetParameter is realtime- and thread-safe (no ring needed).
            AudioUnitSetParameter( unit_, (AudioUnitParameterID) id,
                                   kAudioUnitScope_Global, 0,
                                   (AudioUnitParameterValue) v, 0 );
            return;
        }
    }
}

std::string twAuPlugin::unitSuffixText( std::uint32_t id, double v ) const
{
    const char *suffix = nullptr;
    for( std::size_t i = 0; i < paramIds_.size(); ++i ) {
        if( paramIds_[i] != (AudioUnitParameterID) id )
            continue;
        switch( paramUnits_[i] ) {
            case kAudioUnitParameterUnit_Decibels:       suffix = " dB";  break;
            case kAudioUnitParameterUnit_Hertz:          suffix = " Hz";  break;
            case kAudioUnitParameterUnit_Percent:        suffix = " %";   break;
            case kAudioUnitParameterUnit_Milliseconds:   suffix = " ms";  break;
            case kAudioUnitParameterUnit_Seconds:        suffix = " s";   break;
            case kAudioUnitParameterUnit_Cents:          suffix = " ct";  break;
            case kAudioUnitParameterUnit_SampleFrames:   suffix = " smp"; break;
            case kAudioUnitParameterUnit_BPM:            suffix = " BPM"; break;
            case kAudioUnitParameterUnit_Degrees:        suffix = " deg"; break;
            default: break;  // Indexed/Boolean/Generic/unknown: let the host format
        }
        break;
    }
    if( !suffix )
        return {};
    char buf[64];
    std::snprintf( buf, sizeof( buf ), "%g%s", v, suffix );
    return std::string( buf );
}

std::string twAuPlugin::paramValueText( std::uint32_t id, double v ) const
{
    // On the UI/main thread (like readParams); no hostMutex_ (that guards
    // initialize/uninitialize only). `v` is the parameter's native domain.
    if( !unit_ )
        return {};

    // Primary: ask the AU to format the value in its own units/enum names.
    Float32                            fv  = (Float32) v;
    AudioUnitParameterStringFromValue  req;
    std::memset( &req, 0, sizeof( req ) );
    req.inParamID = (AudioUnitParameterID) id;
    req.inValue   = &fv;
    req.outString = nullptr;
    UInt32 sz = sizeof( req );
    if( AudioUnitGetProperty( unit_, kAudioUnitProperty_ParameterStringFromValue,
                              kAudioUnitScope_Global, id, &req, &sz ) == noErr
        && req.outString ) {
        const CFIndex len = CFStringGetLength( req.outString );
        std::string   out( (std::size_t) len * 4 + 1, '\0' );
        std::string   result;
        if( CFStringGetCString( req.outString, &out[0], (CFIndex) out.size(),
                                kCFStringEncodingUTF8 ) ) {
            out.resize( std::char_traits<char>::length( out.c_str() ) );
            result = out;
        }
        CFRelease( req.outString );
        if( !result.empty() )
            return result;
    }

    // Fallback: many AUs implement no ParameterStringFromValue. Synthesize a
    // suffix from the unit the AU always reports (empty for indexed/boolean).
    return unitSuffixText( id, v );
}

// --- state ------------------------------------------------------------------

std::vector<std::uint8_t> twAuPlugin::saveState() const
{
    std::vector<std::uint8_t> blob( kStateHeaderSize, 0 );
    std::memcpy( blob.data(), kStateMagic, sizeof( kStateMagic ) );
    putU16le( blob.data() + 4, kStateVersion );
    putU16le( blob.data() + 6, 0 );

    if( !unit_ )
        return blob;

    CFPropertyListRef plist = nullptr;
    UInt32            sz     = sizeof( plist );
    if( AudioUnitGetProperty( unit_, kAudioUnitProperty_ClassInfo,
                              kAudioUnitScope_Global, 0, &plist, &sz ) != noErr
        || !plist )
        return blob;   // header only

    CFDataRef data = CFPropertyListCreateData( kCFAllocatorDefault, plist,
                                               kCFPropertyListBinaryFormat_v1_0, 0,
                                               nullptr );
    if( data ) {
        const UInt8 *bytes = CFDataGetBytePtr( data );
        const CFIndex len  = CFDataGetLength( data );
        blob.insert( blob.end(), bytes, bytes + len );
        CFRelease( data );
    } else {
        TW_LOGW( "plugins", "[au] ClassInfo serialization failed for '%s'; "
                 "storing an empty chunk", uid_.c_str() );
    }
    CFRelease( plist );
    return blob;
}

bool twAuPlugin::loadState( const std::vector<std::uint8_t> &blob )
{
    if( blob.size() < kStateHeaderSize )
        return false;
    if( std::memcmp( blob.data(), kStateMagic, sizeof( kStateMagic ) ) != 0 ) {
        TW_LOGW( "plugins", "[au] state blob for '%s' has a foreign magic", uid_.c_str() );
        return false;
    }
    const std::uint16_t ver = getU16le( blob.data() + 4 );
    if( ver > kStateVersion ) {
        TW_LOGW( "plugins", "[au] state blob for '%s' is version %u, we understand %u",
                 uid_.c_str(), (unsigned) ver, (unsigned) kStateVersion );
        return false;
    }

    const std::size_t payload = blob.size() - kStateHeaderSize;
    if( payload == 0 )
        return true;
    if( !unit_ )
        return false;

    CFDataRef data = CFDataCreate( kCFAllocatorDefault, blob.data() + kStateHeaderSize,
                                   (CFIndex) payload );
    if( !data )
        return false;

    CFPropertyListRef plist = CFPropertyListCreateWithData(
        kCFAllocatorDefault, data, kCFPropertyListImmutable, nullptr, nullptr );
    CFRelease( data );
    if( !plist ) {
        TW_LOGW( "plugins", "[au] ClassInfo blob for '%s' is not a property list",
                 uid_.c_str() );
        return false;
    }

    const OSStatus st = AudioUnitSetProperty( unit_, kAudioUnitProperty_ClassInfo,
                                              kAudioUnitScope_Global, 0, &plist,
                                              sizeof( plist ) );
    CFRelease( plist );
    if( st != noErr ) {
        TW_LOGW( "plugins", "[au] ClassInfo restore failed for '%s' (err %d)",
                 uid_.c_str(), (int) st );
        return false;
    }
    return true;
}

// --- factory (referenced by name from twPluginRegistry::instantiate) --------

std::unique_ptr<twPlugin> createAuPlugin( const std::string & /*path*/,
                                          const std::string &uid )
{
    return twAuPlugin::create( uid );
}

}  // namespace audio
