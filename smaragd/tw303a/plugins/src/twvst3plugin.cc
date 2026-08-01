// VST3 backend: audio::twPlugin over IComponent + IAudioProcessor +
// IEditController (proposal 08 M6).
//
// Only compiled when the vst3_pluginterfaces submodule is present (see the VST3
// block in tw303a/CMakeLists.txt), so there is no TW_HAVE_VST3 #ifdef in here.
//
// Threading, and why setParam() does not touch the processor
// ----------------------------------------------------------
// This is the same shape as the CLAP backend, for the same reason, but VST3
// makes the trap sharper: IEditController::setParamNormalized updates the
// CONTROLLER only. It never reaches IAudioProcessor. A host that stops there has
// a parameter UI that moves and audio that does not change — the single most
// common VST3 host bug, and what 08_PLUGIN_HOSTING_EXECUTION.md M6 calls out.
//
// The only route to the DSP is ProcessData::inputParameterChanges. So setParam()
//   1. updates a host-side mirror (which is what getParam() reads),
//   2. pushes a (id, normalized) pair into a lock-free single-producer ring, and
//   3. tells the controller too, so a native editor and the automation view agree.
// process() drains the ring into twVst3ParamChanges. Ring overflow is not a lost
// edit: it raises resyncAll_ and the next drain re-sends every parameter from the
// mirror — parameters are last-value-wins, so a full resync always substitutes
// for a backlog.
//
// PARAMETER DOMAIN. VST3 parameters are NORMALIZED to [0,1] at the interface, and
// that is exactly what this backend exposes: twPluginParamInfo carries
// min = 0, max = 1, default = defaultNormalizedValue. Converting to plain units
// (dB, Hz) would need IEditController::normalizedParamToPlain on every read and a
// non-monotonic inverse for stepped parameters, and would put a controller call
// on the UI thread's hot path for no gain — the generic editor draws a slider
// either way, and automation is normalized at every other layer too.

#include "twvst3host.h"
#include "twvst3module.h"

#include "tw/core/twlog.h"

#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/vstspeaker.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

using namespace Steinberg;

namespace audio {

namespace {

// Our own frame around the plugin's opaque state. CONTRACT invariant 3: blobs
// round-trip through project files, are versioned, and a newer version is
// REFUSED rather than guessed at.
//
// Distinct magic from the CLAP backend's 'TWCP' on purpose — a blob must never
// be silently interpretable by the wrong backend. VST3 state is TWO chunks
// (component and controller), so unlike CLAP the payload is length-prefixed
// rather than "everything after the header".
constexpr std::size_t   kStateHeaderSize = 8;
constexpr std::uint8_t  kStateMagic[4]   = { 'T', 'W', 'V', '3' };
constexpr std::uint16_t kStateVersion    = 1;

void putU16le( std::uint8_t *p, std::uint16_t v )
{
    p[0] = (std::uint8_t)( v & 0xFF );
    p[1] = (std::uint8_t)( ( v >> 8 ) & 0xFF );
}

std::uint16_t getU16le( const std::uint8_t *p )
{
    return (std::uint16_t)( (std::uint16_t)p[0] | ( (std::uint16_t)p[1] << 8 ) );
}

void putU32le( std::uint8_t *p, std::uint32_t v )
{
    p[0] = (std::uint8_t)( v & 0xFF );
    p[1] = (std::uint8_t)( ( v >> 8 ) & 0xFF );
    p[2] = (std::uint8_t)( ( v >> 16 ) & 0xFF );
    p[3] = (std::uint8_t)( ( v >> 24 ) & 0xFF );
}

std::uint32_t getU32le( const std::uint8_t *p )
{
    return (std::uint32_t)p[0] | ( (std::uint32_t)p[1] << 8 ) |
           ( (std::uint32_t)p[2] << 16 ) | ( (std::uint32_t)p[3] << 24 );
}

void appendChunk( std::vector<std::uint8_t> &blob, const std::vector<std::uint8_t> &c )
{
    const std::size_t at = blob.size();
    blob.resize( at + 4 );
    putU32le( blob.data() + at, (std::uint32_t)c.size() );
    blob.insert( blob.end(), c.begin(), c.end() );
}

// UTF-16 String128 to UTF-8, enough for a parameter title.
std::string u16ToUtf8( const Vst::TChar *s, std::size_t maxLen = 128 )
{
    std::string out;
    if( !s ) return out;
    for( std::size_t i = 0; i < maxLen && s[i]; ++i ) {
        const std::uint32_t c = (std::uint32_t)(std::uint16_t)s[i];
        if( c < 0x80 ) {
            out.push_back( (char)c );
        } else if( c < 0x800 ) {
            out.push_back( (char)( 0xC0 | ( c >> 6 ) ) );
            out.push_back( (char)( 0x80 | ( c & 0x3F ) ) );
        } else {
            out.push_back( (char)( 0xE0 | ( c >> 12 ) ) );
            out.push_back( (char)( 0x80 | ( ( c >> 6 ) & 0x3F ) ) );
            out.push_back( (char)( 0x80 | ( c & 0x3F ) ) );
        }
    }
    return out;
}

Vst::SpeakerArrangement arrangementFor( int32 channels )
{
    switch( channels ) {
        case 0:  return 0;
        case 1:  return Vst::SpeakerArr::kMono;
        case 2:  return Vst::SpeakerArr::kStereo;
        default: {
            // A dense low-order bitset is the honest generic answer: the exact
            // speaker identities do not matter to us, only the count.
            Vst::SpeakerArrangement a = 0;
            for( int32 i = 0; i < channels && i < 64; ++i )
                a |= (Vst::SpeakerArrangement)1 << i;
            return a;
        }
    }
}

}  // namespace

class twVst3Plugin final : public twPlugin {
public:
    static std::unique_ptr<twVst3Plugin> create( const std::string &path,
                                                 const std::string &uid );
    ~twVst3Plugin() override;

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

    std::uint32_t reportedLatency() const override;
    bool supportsNativeEditor() const override { return hasGui_; }
    bool acceptsNotes()         const override { return acceptsNotes_; }

private:
    twVst3Plugin() = default;

    bool init( const std::string &path, const std::string &uid );
    void teardown();
    void deactivate();                 // hostMutex_ must be held
    void readBusLayout();
    void readParams();
    void refreshMirror();              // main thread

    struct BusShape {
        int32 channels = 0;
        bool  isMain   = false;
    };

    void pushEdit( std::uint32_t id, double v );
    void drainEditsIntoChanges();      // fills paramChanges_

    std::shared_ptr<twVst3Module> module_;
    Vst::IComponent              *component_  = nullptr;
    Vst::IAudioProcessor         *processor_  = nullptr;
    Vst::IEditController         *controller_ = nullptr;
    Vst::IConnectionPoint        *cpComp_     = nullptr;
    Vst::IConnectionPoint        *cpCtrl_     = nullptr;
    bool                          controllerIsSeparate_ = false;

    std::unique_ptr<twVst3HostApplication>  host_;
    std::unique_ptr<twVst3ComponentHandler> handler_;

    twPluginIoLayout io_{};
    std::string      uid_, path_;
    bool             hasGui_       = false;
    bool             acceptsNotes_ = false;

    // Every declared bus, not just main: VST3's process() expects one
    // AudioBusBuffers per declared bus with a matching channel count, even for
    // an inactive one. Short-changing it is a spec violation, so the non-main
    // buses get zeroed input scratch and throwaway output scratch.
    std::vector<BusShape> inBusShape_, outBusShape_;
    int                   mainInBus_ = -1, mainOutBus_ = -1;

    std::vector<Vst::AudioBusBuffers>  inBufs_, outBufs_;
    std::vector<std::vector<float>>    scratchOwned_;
    std::vector<std::vector<float *>>  inBusPtrs_, outBusPtrs_;
    std::vector<float *>               mainInPtrs_, mainOutPtrs_;

    std::vector<twPluginParamInfo> params_;
    std::vector<Vst::ParamID>      paramIds_;
    std::unique_ptr<std::atomic<double>[]> mirror_;

    struct ParamEdit {
        std::uint32_t id;
        double        value;
    };
    static constexpr std::uint32_t kRingCapacity = 256;   // power of two
    ParamEdit                  ring_[kRingCapacity];
    std::atomic<std::uint32_t> ringWrite_{ 0 }, ringRead_{ 0 };
    std::atomic<bool>          resyncAll_{ false };

    twVst3ParamChanges paramChanges_;

    // hostMutex_ serializes the activation TRANSITIONS. process() never takes
    // it — it only reads the flags, which is why they are atomic.
    mutable std::mutex         hostMutex_;
    std::atomic<bool>          active_{ false };
    std::atomic<bool>          processing_{ false };
    std::atomic<bool>          processFailed_{ false };
    std::atomic<std::uint32_t> preparedMax_{ 0 };
    std::uint32_t              preparedRate_ = 0;   // hostMutex_ only
};

// --- construction -------------------------------------------------------------

std::unique_ptr<twVst3Plugin> twVst3Plugin::create( const std::string &path,
                                                    const std::string &uid )
{
    std::unique_ptr<twVst3Plugin> p( new twVst3Plugin() );
    if( !p->init( path, uid ) ) return nullptr;
    return p;
}

twVst3Plugin::~twVst3Plugin()
{
    teardown();
}

bool twVst3Plugin::init( const std::string &path, const std::string &uid )
{
    path_ = path;
    uid_  = uid;

    module_ = twVst3Module::open( path );
    if( !module_ || !module_->factory() ) return false;

    IPluginFactory *factory = module_->factory();

    // Find the requested class (empty uid = the first audio effect).
    PClassInfo want{};
    bool       found = false;
    const int32 n = factory->countClasses();
    for( int32 i = 0; i < n && !found; ++i ) {
        PClassInfo ci{};
        if( factory->getClassInfo( i, &ci ) != kResultOk ) continue;
        if( std::strncmp( ci.category, kVstAudioEffectClass,
                          PClassInfo::kCategorySize ) != 0 )
            continue;
        if( uid.empty() || vst3UidFromTuid( ci.cid ) == uid ) {
            want  = ci;
            found = true;
        }
    }
    if( !found ) {
        TW_LOGE( "plugins", "[vst3] '%s' has no audio-effect class with uid '%s'",
                 path.c_str(), uid.c_str() );
        return false;
    }
    if( uid_.empty() ) uid_ = vst3UidFromTuid( want.cid );

    host_.reset( new twVst3HostApplication( "Smaragd" ) );
    handler_.reset( new twVst3ComponentHandler() );

    if( factory->createInstance( want.cid, Vst::IComponent::iid,
                                 (void **)&component_ ) != kResultOk || !component_ ) {
        TW_LOGE( "plugins", "[vst3] createInstance(IComponent) failed for '%s'", uid_.c_str() );
        return false;
    }
    if( component_->initialize( host_.get() ) != kResultOk ) {
        TW_LOGE( "plugins", "[vst3] IComponent::initialize failed for '%s'", uid_.c_str() );
        return false;
    }
    if( component_->queryInterface( Vst::IAudioProcessor::iid,
                                    (void **)&processor_ ) != kResultOk || !processor_ ) {
        TW_LOGE( "plugins", "[vst3] '%s' offers no IAudioProcessor", uid_.c_str() );
        return false;
    }
    if( processor_->canProcessSampleSize( Vst::kSample32 ) != kResultTrue ) {
        TW_LOGE( "plugins", "[vst3] '%s' refuses 32-bit float processing; the engine "
                 "has no 64-bit path", uid_.c_str() );
        return false;
    }

    // Two shapes exist and both are common: a single-component plugin whose
    // IComponent also answers IEditController, and the classic split pair
    // reached through getControllerClassId.
    if( component_->queryInterface( Vst::IEditController::iid,
                                    (void **)&controller_ ) != kResultOk )
        controller_ = nullptr;
    if( !controller_ ) {
        TUID ccid;
        if( component_->getControllerClassId( ccid ) == kResultOk ) {
            if( factory->createInstance( ccid, Vst::IEditController::iid,
                                         (void **)&controller_ ) == kResultOk && controller_ ) {
                controllerIsSeparate_ = true;
                if( controller_->initialize( host_.get() ) != kResultOk ) {
                    TW_LOGW( "plugins", "[vst3] IEditController::initialize failed for '%s'; "
                             "continuing without a controller", uid_.c_str() );
                    controller_->release();
                    controller_           = nullptr;
                    controllerIsSeparate_ = false;
                }
            }
        }
    }

    if( controller_ ) {
        controller_->setComponentHandler( handler_.get() );

        // Seed the controller from the component's state, as a host must — this
        // is what makes the editor agree with the DSP at instantiation.
        twVst3MemStream seed;
        if( component_->getState( &seed ) == kResultOk ) {
            seed.rewind();
            controller_->setComponentState( &seed );
        }

        // Connect the pair DIRECTLY. In-process there is no reason to mediate
        // their messages; the host still has to be able to manufacture an
        // IMessage for them (twVst3HostApplication::createInstance), which is
        // what they will ask us for.
        if( controllerIsSeparate_ ) {
            if( component_->queryInterface( Vst::IConnectionPoint::iid,
                                            (void **)&cpComp_ ) != kResultOk )
                cpComp_ = nullptr;
            if( controller_->queryInterface( Vst::IConnectionPoint::iid,
                                             (void **)&cpCtrl_ ) != kResultOk )
                cpCtrl_ = nullptr;
            if( cpComp_ && cpCtrl_ ) {
                cpComp_->connect( cpCtrl_ );
                cpCtrl_->connect( cpComp_ );
            }
        }

        // The only accurate answer to supportsNativeEditor() is to ask for the
        // view and let go of it again; there is no capability query. Native
        // editor windows are a documented deferral, so a false positive costs
        // nothing and a false negative would misreport the plugin.
        if( IPlugView *v = controller_->createView( Vst::ViewType::kEditor ) ) {
            hasGui_ = true;
            v->release();
        }
    }

    readBusLayout();
    readParams();

    // An instrument declares an event input bus. Notes are out of scope until
    // the MIDI/note model exists (proposal 08 §Known deferrals), but reporting
    // it honestly is what lets the UI say so.
    acceptsNotes_ = component_->getBusCount( Vst::kEvent, Vst::kInput ) > 0;

    TW_LOGI( "plugins", "[vst3] instantiated '%s' (%u in / %u out, %llu params%s)",
             uid_.c_str(), (unsigned)io_.audioInputs, (unsigned)io_.audioOutputs,
             (unsigned long long)params_.size(), hasGui_ ? ", native editor" : "" );
    return true;
}

void twVst3Plugin::teardown()
{
    {
        std::lock_guard<std::mutex> lock( hostMutex_ );
        deactivate();
    }

    // Documented order. Getting it wrong leaks the DSO or crashes on exit.
    if( cpComp_ && cpCtrl_ ) {
        cpComp_->disconnect( cpCtrl_ );
        cpCtrl_->disconnect( cpComp_ );
    }
    if( cpCtrl_ ) { cpCtrl_->release(); cpCtrl_ = nullptr; }
    if( cpComp_ ) { cpComp_->release(); cpComp_ = nullptr; }

    if( controller_ ) {
        controller_->setComponentHandler( nullptr );
        if( controllerIsSeparate_ ) controller_->terminate();
        controller_->release();
        controller_ = nullptr;
    }
    if( processor_ ) { processor_->release(); processor_ = nullptr; }
    if( component_ ) {
        component_->terminate();
        component_->release();
        component_ = nullptr;
    }
    // host_/handler_ outlive every plugin call by construction: they are
    // destroyed here, after the plugin objects that borrow them are gone.
    handler_.reset();
    host_.reset();
    module_.reset();
}

// --- layout -------------------------------------------------------------------

void twVst3Plugin::readBusLayout()
{
    inBusShape_.clear();
    outBusShape_.clear();
    mainInBus_ = mainOutBus_ = -1;

    for( int dir = 0; dir < 2; ++dir ) {
        const Vst::BusDirection d = dir == 0 ? Vst::kInput : Vst::kOutput;
        std::vector<BusShape>  &dst = dir == 0 ? inBusShape_ : outBusShape_;
        int                    &main = dir == 0 ? mainInBus_ : mainOutBus_;

        const int32 n = component_->getBusCount( Vst::kAudio, d );
        for( int32 i = 0; i < n; ++i ) {
            Vst::BusInfo bi{};
            if( component_->getBusInfo( Vst::kAudio, d, i, bi ) != kResultOk ) {
                dst.push_back( BusShape{ 0, false } );
                continue;
            }
            // A wild channel count is the first symptom of an ABI/struct-layout
            // mismatch; clamp rather than allocate gigabytes from it.
            int32 ch = bi.channelCount;
            if( ch < 0 || ch > 64 ) {
                TW_LOGW( "plugins", "[vst3] '%s' reports %d channels on %s bus %d; clamping",
                         uid_.c_str(), (int)ch, dir == 0 ? "input" : "output", (int)i );
                ch = std::max<int32>( 0, std::min<int32>( ch, 64 ) );
            }
            const bool isMain = bi.busType == Vst::kMain;
            if( isMain && main < 0 ) main = (int)i;
            dst.push_back( BusShape{ ch, isMain } );
        }
    }

    io_.audioInputs  = (std::uint16_t)( mainInBus_  >= 0 ? inBusShape_[(std::size_t)mainInBus_].channels : 0 );
    io_.audioOutputs = (std::uint16_t)( mainOutBus_ >= 0 ? outBusShape_[(std::size_t)mainOutBus_].channels : 0 );
}

void twVst3Plugin::readParams()
{
    params_.clear();
    paramIds_.clear();
    if( !controller_ ) {
        mirror_.reset( new std::atomic<double>[1] );
        return;
    }

    const int32 n = controller_->getParameterCount();
    params_.reserve( (std::size_t)std::max<int32>( 0, n ) );
    paramIds_.reserve( (std::size_t)std::max<int32>( 0, n ) );

    for( int32 i = 0; i < n; ++i ) {
        Vst::ParameterInfo pi{};
        if( controller_->getParameterInfo( i, pi ) != kResultOk ) continue;

        twPluginParamInfo o;
        o.id   = (std::uint32_t)pi.id;
        o.name = u16ToUtf8( pi.title );
        // Normalized domain — see the header comment on PARAMETER DOMAIN.
        o.minValue     = 0.0;
        o.maxValue     = 1.0;
        o.defaultValue = pi.defaultNormalizedValue;
        o.isStepped    = pi.stepCount > 0;

        params_.push_back( std::move( o ) );
        paramIds_.push_back( pi.id );
    }

    mirror_.reset( new std::atomic<double>[ params_.empty() ? 1 : params_.size() ] );
    refreshMirror();
}

void twVst3Plugin::refreshMirror()
{
    for( std::size_t i = 0; i < params_.size(); ++i ) {
        double v = params_[i].defaultValue;
        if( controller_ ) v = controller_->getParamNormalized( paramIds_[i] );
        mirror_[i].store( v, std::memory_order_relaxed );
    }
}

// --- lifecycle ----------------------------------------------------------------

void twVst3Plugin::prepare( std::uint32_t sampleRate, std::uint32_t maxBlock )
{
    if( !component_ || !processor_ ) return;
    if( maxBlock == 0 ) maxBlock = 1;

    std::lock_guard<std::mutex> lock( hostMutex_ );

    if( active_.load( std::memory_order_relaxed ) && preparedRate_ == sampleRate &&
        preparedMax_.load( std::memory_order_relaxed ) == maxBlock )
        return;   // idempotent: the steady-state call is a comparison

    deactivate();

    // Ask for the arrangement we actually intend to drive, BEFORE
    // setupProcessing: a plugin may legally report different bus channel counts
    // afterwards, which is why the layout is re-read below.
    {
        std::vector<Vst::SpeakerArrangement> ins, outs;
        for( const BusShape &b : inBusShape_ )  ins.push_back( arrangementFor( b.channels ) );
        for( const BusShape &b : outBusShape_ ) outs.push_back( arrangementFor( b.channels ) );
        const tresult r = processor_->setBusArrangements(
            ins.empty() ? nullptr : ins.data(), (int32)ins.size(),
            outs.empty() ? nullptr : outs.data(), (int32)outs.size() );
        if( r != kResultOk )
            TW_LOGD( "plugins", "[vst3] '%s' declined our bus arrangement; using its own",
                     uid_.c_str() );

        // ioLayout() MUST NOT move after construction. twPluginSlotProcessor
        // derives the channel-mismatch mapping (Direct / DualMono / MonoFold)
        // from it once, at setBusCount() — i.e. before this runs — and sizes its
        // own buffers from it. A plugin that renegotiated its channel count here
        // would leave that mapping describing a shape the plugin no longer has,
        // and the mismatch would show up as silence or as reads past the end of
        // the processor's scratch rather than as an error. We only ever ask for
        // what the plugin already reported, so this should be unreachable —
        // which is exactly why it is worth refusing loudly instead of trusting.
        const twPluginIoLayout before = io_;
        readBusLayout();
        if( io_.audioInputs != before.audioInputs ||
            io_.audioOutputs != before.audioOutputs ) {
            TW_LOGE( "plugins", "[vst3] '%s' renegotiated its I/O at prepare (%u/%u -> "
                     "%u/%u); the host's channel mapping is already fixed, so this "
                     "instance will pass audio through unchanged",
                     uid_.c_str(), (unsigned)before.audioInputs,
                     (unsigned)before.audioOutputs, (unsigned)io_.audioInputs,
                     (unsigned)io_.audioOutputs );
            io_ = before;
            processFailed_.store( true, std::memory_order_release );
            return;
        }
    }

    Vst::ProcessSetup setup{};
    setup.processMode        = Vst::kRealtime;
    setup.symbolicSampleSize = Vst::kSample32;
    setup.maxSamplesPerBlock = (int32)maxBlock;
    setup.sampleRate         = (double)sampleRate;
    if( processor_->setupProcessing( setup ) != kResultOk ) {
        TW_LOGE( "plugins", "[vst3] setupProcessing(%u Hz, max %u) failed for '%s'; "
                 "the insert will pass audio through unchanged",
                 (unsigned)sampleRate, (unsigned)maxBlock, uid_.c_str() );
        processFailed_.store( true, std::memory_order_release );
        return;
    }
    preparedRate_ = sampleRate;
    preparedMax_.store( maxBlock, std::memory_order_release );
    processFailed_.store( false, std::memory_order_release );

    // Main buses on, everything else off: we have no sidechain routing, and an
    // aux bus left active would have the plugin expecting audio we never supply.
    // Buffers are still handed over for inactive buses — the spec requires the
    // array and the channel count to match regardless.
    for( std::size_t i = 0; i < inBusShape_.size(); ++i )
        component_->activateBus( Vst::kAudio, Vst::kInput, (int32)i,
                                 (int)i == mainInBus_ );
    for( std::size_t i = 0; i < outBusShape_.size(); ++i )
        component_->activateBus( Vst::kAudio, Vst::kOutput, (int32)i,
                                 (int)i == mainOutBus_ );

    // --- buffers. Allocated here, never in process(). ------------------------
    //
    // One flat scratch pool so the per-channel float* stay stable: growing a
    // vector<vector<float>> later would invalidate every pointer handed to the
    // plugin.
    std::size_t scratchChannels = 0;
    for( std::size_t i = 0; i < inBusShape_.size(); ++i )
        if( (int)i != mainInBus_ ) scratchChannels += (std::size_t)inBusShape_[i].channels;
    for( std::size_t i = 0; i < outBusShape_.size(); ++i )
        if( (int)i != mainOutBus_ ) scratchChannels += (std::size_t)outBusShape_[i].channels;

    scratchOwned_.assign( scratchChannels, std::vector<float>( maxBlock, 0.0f ) );
    std::size_t nextScratch = 0;

    auto buildBuses = [&]( const std::vector<BusShape> &shape, int mainBus,
                           std::vector<std::vector<float *>> &ptrs,
                           std::vector<Vst::AudioBusBuffers> &bufs ) {
        ptrs.assign( shape.size(), std::vector<float *>{} );
        bufs.assign( shape.size(), Vst::AudioBusBuffers{} );
        for( std::size_t b = 0; b < shape.size(); ++b ) {
            bufs[b].numChannels      = shape[b].channels;
            bufs[b].silenceFlags     = 0;
            bufs[b].channelBuffers32 = nullptr;
            if( (int)b == mainBus ) {
                ptrs[b].assign( (std::size_t)shape[b].channels, nullptr );
                continue;   // filled per call from the caller's pointers
            }
            ptrs[b].resize( (std::size_t)shape[b].channels );
            for( int32 c = 0; c < shape[b].channels; ++c )
                ptrs[b][(std::size_t)c] = scratchOwned_[nextScratch++].data();
            bufs[b].channelBuffers32 = ptrs[b].empty() ? nullptr : ptrs[b].data();
        }
    };
    buildBuses( inBusShape_,  mainInBus_,  inBusPtrs_,  inBufs_ );
    buildBuses( outBusShape_, mainOutBus_, outBusPtrs_, outBufs_ );

    mainInPtrs_.assign( io_.audioInputs, nullptr );
    mainOutPtrs_.assign( io_.audioOutputs, nullptr );

    // Worst case per drain is one point per parameter (last-value-wins within a
    // block), plus headroom for a ring's worth landing on one parameter.
    paramChanges_.reserve( params_.size(), 8 );

    // Documented order: setActive(true) first, then setProcessing(true).
    if( component_->setActive( true ) != kResultOk ) {
        TW_LOGE( "plugins", "[vst3] setActive(true) failed for '%s'; the insert will "
                 "pass audio through unchanged", uid_.c_str() );
        processFailed_.store( true, std::memory_order_release );
        return;
    }
    if( processor_->setProcessing( true ) != kResultOk ) {
        // Legal to decline; the plugin still processes. Note it and carry on.
        TW_LOGD( "plugins", "[vst3] '%s' declined setProcessing(true)", uid_.c_str() );
    }
    processing_.store( true, std::memory_order_release );

    // Published last, with release semantics: process() reads active_ without
    // the mutex, so every buffer it will touch must already be visible.
    active_.store( true, std::memory_order_release );
}

// hostMutex_ must be held.
void twVst3Plugin::deactivate()
{
    if( !component_ ) return;
    const bool wasActive = active_.exchange( false, std::memory_order_acq_rel );
    if( processing_.exchange( false, std::memory_order_acq_rel ) && processor_ )
        processor_->setProcessing( false );
    if( wasActive ) component_->setActive( false );
}

void twVst3Plugin::reset()
{
    // VST3 has no reset(): the documented way to flush a plugin's tails and
    // internal state is the deactivate/activate cycle. Only meaningful once
    // prepared — an unprepared instance has nothing to flush.
    std::lock_guard<std::mutex> lock( hostMutex_ );
    if( !component_ || !active_.load( std::memory_order_acquire ) ) return;

    if( processing_.exchange( false, std::memory_order_acq_rel ) && processor_ )
        processor_->setProcessing( false );
    component_->setActive( false );
    if( component_->setActive( true ) != kResultOk ) {
        TW_LOGW( "plugins", "[vst3] reset: re-activation failed for '%s'", uid_.c_str() );
        active_.store( false, std::memory_order_release );
        processFailed_.store( true, std::memory_order_release );
        return;
    }
    if( processor_ ) processor_->setProcessing( true );
    processing_.store( true, std::memory_order_release );
}

std::uint32_t twVst3Plugin::reportedLatency() const
{
    return processor_ ? (std::uint32_t)processor_->getLatencySamples() : 0;
}

// --- parameters ---------------------------------------------------------------

twPluginParamInfo twVst3Plugin::paramInfo( std::size_t i ) const
{
    if( i < params_.size() ) return params_[i];
    return twPluginParamInfo{};
}

double twVst3Plugin::getParam( std::uint32_t id ) const
{
    for( std::size_t i = 0; i < params_.size(); ++i )
        if( params_[i].id == id )
            return mirror_[i].load( std::memory_order_acquire );
    return 0.0;
}

std::string twVst3Plugin::paramValueText( std::uint32_t id, double v ) const
{
    // IEditController lives on the UI/edit-controller thread (= the Qt UI thread),
    // so this is called off the audio path with no lock — like refreshMirror()'s
    // getParamNormalized(). `v` is already NORMALIZED [0,1], which is exactly
    // getParamStringByValue()'s input contract, so no conversion is needed.
    if( !controller_ )
        return {};
    Vst::String128 s{};
    if( controller_->getParamStringByValue( (Vst::ParamID)id, v, s ) != kResultOk )
        return {};
    return u16ToUtf8( s );
}

void twVst3Plugin::setParam( std::uint32_t id, double v )
{
    std::size_t idx = params_.size();
    for( std::size_t i = 0; i < params_.size(); ++i ) {
        if( params_[i].id == id ) {
            idx = i;
            break;
        }
    }
    if( idx == params_.size() ) return;

    if( v < 0.0 ) v = 0.0;
    if( v > 1.0 ) v = 1.0;

    mirror_[idx].store( v, std::memory_order_release );
    pushEdit( id, v );

    // Tell the controller too. This does NOT reach the processor — that is what
    // the ring is for — but without it a native editor and
    // getParamStringByValue would disagree with the audio.
    if( controller_ ) controller_->setParamNormalized( (Vst::ParamID)id, v );
}

void twVst3Plugin::pushEdit( std::uint32_t id, double v )
{
    const std::uint32_t w = ringWrite_.load( std::memory_order_relaxed );
    const std::uint32_t r = ringRead_.load( std::memory_order_acquire );
    if( w - r >= kRingCapacity ) {
        // Backlog. Do not drop silently and do not touch ringRead_ from the
        // producer side: raise the resync flag and let the next drain re-send
        // every parameter from the mirror. Last-value-wins makes that strictly
        // better than replaying the backlog.
        resyncAll_.store( true, std::memory_order_release );
        return;
    }
    ring_[w & ( kRingCapacity - 1 )] = ParamEdit{ id, v };
    ringWrite_.store( w + 1, std::memory_order_release );
}

void twVst3Plugin::drainEditsIntoChanges()
{
    paramChanges_.clearAll();

    auto emit = [this]( std::uint32_t id, double v ) {
        int32 qIndex = 0;
        if( Vst::IParamValueQueue *q =
                paramChanges_.addParameterData( (Vst::ParamID)id, qIndex ) ) {
            int32 pIndex = 0;
            q->addPoint( 0, v, pIndex );   // sample-accurate ramps are future work
        }
    };

    const std::uint32_t w = ringWrite_.load( std::memory_order_acquire );
    std::uint32_t       r = ringRead_.load( std::memory_order_relaxed );

    if( resyncAll_.exchange( false, std::memory_order_acq_rel ) ) {
        // Everything the ring still holds is subsumed by the mirror.
        ringRead_.store( w, std::memory_order_release );
        for( std::size_t i = 0; i < params_.size(); ++i )
            emit( params_[i].id, mirror_[i].load( std::memory_order_acquire ) );
    } else {
        for( ; r != w; ++r ) {
            const ParamEdit &e = ring_[r & ( kRingCapacity - 1 )];
            emit( e.id, e.value );
        }
        ringRead_.store( w, std::memory_order_release );
    }
}

// --- processing ---------------------------------------------------------------

void twVst3Plugin::process( const float *const *in, float *const *out,
                            std::uint32_t nframes )
{
    const std::uint32_t nIn  = io_.audioInputs;
    const std::uint32_t nOut = io_.audioOutputs;

    auto passThrough = [&]() {
        for( std::uint32_t c = 0; c < nOut; ++c ) {
            if( !out[c] ) continue;
            if( c < nIn && in && in[c] )
                std::memcpy( out[c], in[c], (std::size_t)nframes * sizeof( float ) );
            else
                std::memset( out[c], 0, (std::size_t)nframes * sizeof( float ) );
        }
    };

    if( !processor_ || nframes == 0 ||
        processFailed_.load( std::memory_order_acquire ) ||
        !active_.load( std::memory_order_acquire ) ) {
        passThrough();
        return;
    }
    const std::uint32_t maxBlock = preparedMax_.load( std::memory_order_acquire );
    if( nframes > maxBlock ) {
        // The caller must chunk to the value it passed to prepare(); an overlong
        // block would overrun the plugin's own setup-sized buffers.
        TW_LOGE( "plugins", "[vst3] process(%u) exceeds prepared max %u for '%s'; "
                 "passing audio through", (unsigned)nframes, (unsigned)maxBlock,
                 uid_.c_str() );
        passThrough();
        return;
    }

    drainEditsIntoChanges();

    // Point the main buses at the caller's de-interleaved buffers. The input
    // buffers are the host's own scratch (see twPluginInsert), so a plugin that
    // writes them harms nothing.
    for( std::uint32_t c = 0; c < nIn; ++c )
        mainInPtrs_[c] = const_cast<float *>( in ? in[c] : nullptr );
    for( std::uint32_t c = 0; c < nOut; ++c )
        mainOutPtrs_[c] = out ? out[c] : nullptr;

    if( mainInBus_ >= 0 && (std::size_t)mainInBus_ < inBufs_.size() )
        inBufs_[(std::size_t)mainInBus_].channelBuffers32 =
            mainInPtrs_.empty() ? nullptr : mainInPtrs_.data();
    if( mainOutBus_ >= 0 && (std::size_t)mainOutBus_ < outBufs_.size() )
        outBufs_[(std::size_t)mainOutBus_].channelBuffers32 =
            mainOutPtrs_.empty() ? nullptr : mainOutPtrs_.data();

    Vst::ProcessData data{};
    data.processMode            = Vst::kRealtime;
    data.symbolicSampleSize     = Vst::kSample32;
    data.numSamples             = (int32)nframes;
    data.numInputs              = (int32)inBufs_.size();
    data.numOutputs             = (int32)outBufs_.size();
    data.inputs                 = inBufs_.empty() ? nullptr : inBufs_.data();
    data.outputs                = outBufs_.empty() ? nullptr : outBufs_.data();
    data.inputParameterChanges  = &paramChanges_;
    data.outputParameterChanges = nullptr;
    data.inputEvents            = nullptr;   // notes are a documented deferral
    data.outputEvents           = nullptr;
    data.processContext         = nullptr;   // free-running; no tempo map yet

    if( processor_->process( data ) != kResultOk ) {
        TW_LOGE( "plugins", "[vst3] '%s' returned an error from process(); "
                 "disabling processing for this instance", uid_.c_str() );
        processFailed_.store( true, std::memory_order_release );
        passThrough();
    }
}

// --- state --------------------------------------------------------------------

std::vector<std::uint8_t> twVst3Plugin::saveState() const
{
    std::vector<std::uint8_t> blob( kStateHeaderSize, 0 );
    std::memcpy( blob.data(), kStateMagic, sizeof( kStateMagic ) );
    putU16le( blob.data() + 4, kStateVersion );
    putU16le( blob.data() + 6, 0 );   // reserved

    std::vector<std::uint8_t> comp, ctrl;

    if( component_ ) {
        twVst3MemStream s;
        if( component_->getState( &s ) == kResultOk ) comp = s.bytes();
        else TW_LOGW( "plugins", "[vst3] component state save failed for '%s'", uid_.c_str() );
    }
    // ONLY a separate controller has state of its own worth storing. In a
    // single-component plugin the same object implements both interfaces, and
    // IComponent::getState and IEditController::getState have identical
    // signatures — so one override serves both and the plugin has no way to make
    // them differ. Asking twice would store the same payload twice and restore
    // it twice, for nothing.
    if( controller_ && controllerIsSeparate_ ) {
        twVst3MemStream s;
        // A controller may legitimately have nothing of its own to store.
        if( controller_->getState( &s ) == kResultOk ) ctrl = s.bytes();
    }

    appendChunk( blob, comp );
    appendChunk( blob, ctrl );
    return blob;
}

bool twVst3Plugin::loadState( const std::vector<std::uint8_t> &blob )
{
    if( blob.size() < kStateHeaderSize ) return false;
    if( std::memcmp( blob.data(), kStateMagic, sizeof( kStateMagic ) ) != 0 ) {
        TW_LOGW( "plugins", "[vst3] state blob for '%s' has a foreign magic", uid_.c_str() );
        return false;
    }
    const std::uint16_t ver = getU16le( blob.data() + 4 );
    if( ver > kStateVersion ) {
        TW_LOGW( "plugins", "[vst3] state blob for '%s' is version %u, we understand %u; "
                 "keeping the plugin at its defaults", uid_.c_str(), (unsigned)ver,
                 (unsigned)kStateVersion );
        return false;
    }

    // Two length-prefixed chunks. A truncated blob is refused whole rather than
    // half-applied: half a restored patch is worse than none.
    std::size_t at = kStateHeaderSize;
    auto readChunk = [&]( std::vector<std::uint8_t> &out ) -> bool {
        if( at + 4 > blob.size() ) return false;
        const std::uint32_t len = getU32le( blob.data() + at );
        at += 4;
        if( (std::uint64_t)at + len > blob.size() ) return false;
        out.assign( blob.begin() + (std::ptrdiff_t)at,
                    blob.begin() + (std::ptrdiff_t)( at + len ) );
        at += len;
        return true;
    };

    std::vector<std::uint8_t> comp, ctrl;
    if( !readChunk( comp ) || !readChunk( ctrl ) ) {
        TW_LOGW( "plugins", "[vst3] state blob for '%s' is truncated", uid_.c_str() );
        return false;
    }

    bool ok = true;
    if( component_ && !comp.empty() ) {
        twVst3MemStream s( comp );
        if( component_->setState( &s ) != kResultOk ) {
            TW_LOGW( "plugins", "[vst3] component state load failed for '%s'", uid_.c_str() );
            ok = false;
        }
        // The controller must see the COMPONENT's state as well as its own —
        // that is what setComponentState is for, and skipping it leaves the
        // editor showing defaults over restored audio.
        if( controller_ ) {
            s.rewind();
            controller_->setComponentState( &s );
        }
    }
    if( controller_ && !ctrl.empty() ) {
        twVst3MemStream s( ctrl );
        if( controller_->setState( &s ) != kResultOk )
            TW_LOGD( "plugins", "[vst3] controller state load declined by '%s'", uid_.c_str() );
    }

    // The plugin's values just changed underneath the mirror.
    refreshMirror();
    return ok;
}

// --- factory (referenced by name from twPluginRegistry::instantiate) ----------

std::unique_ptr<twPlugin> createVst3Plugin( const std::string &path,
                                            const std::string &uid )
{
    return twVst3Plugin::create( path, uid );
}

}  // namespace audio
