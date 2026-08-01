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

private:
    twAuPlugin() = default;

    bool init( const std::string &uid );
    void uninitialize();                 // hostMutex_ must be held
    void readIoLayout();
    void readParams();
    void readGui();

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

    readIoLayout();
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

void twAuPlugin::process( const float *const *in, float *const *out,
                          std::uint32_t nframes )
{
    const std::uint32_t nIn  = io_.audioInputs;
    const std::uint32_t nOut = io_.audioOutputs;

    auto passThrough = [&]() {
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
