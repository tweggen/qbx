// CLAP backend: audio::twPlugin over clap_plugin_t (proposal 08 M1).
//
// Only compiled when the CLAP submodule is present (see the _tw_clap_found
// block in tw303a/CMakeLists.txt), so there is no TW_HAVE_CLAP #ifdef in here.
//
// Threading, and why setParam() does not touch the plugin
// ------------------------------------------------------
// CLAP annotates every entry point with the thread it may be called from.
// process() and params->flush() are [audio-thread] while the plugin is active;
// everything else is [main-thread]. Our parameter edits originate on the UI
// thread, so calling params->flush() (let alone poking a value) from setParam()
// while a worker is inside process() is a data race by construction.
//
// setParam() therefore only
//   1. updates a host-side mirror (which is what getParam() reads), and
//   2. pushes a (id, value) pair into a lock-free single-producer ring.
// process() drains the ring into clap_process::in_events as
// CLAP_EVENT_PARAM_VALUE events. When the plugin is NOT active (no render can
// be in flight) setParam() drains through params->flush() directly instead, so
// an edit made while the transport is idle is not deferred indefinitely; the
// active_ flag is read under hostMutex_, which prepare()/deactivate also take,
// so that decision cannot race with activation.
//
// Ring overflow is not a lost edit: it raises resyncAll_, and the next drain
// re-sends *every* parameter from the mirror. Parameters are last-value-wins, so
// a full resync is always a correct substitute for a backlog.

#include "twclapmodule.h"

#include "tw/core/twlog.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace audio {

namespace {

// Our own frame around the plugin's opaque state chunk. CONTRACT invariant 3:
// blobs round-trip through project files, are versioned, and tolerate unknown
// trailing data — the CLAP payload IS the trailing data, so the frame is a fixed
// 8 bytes and everything after it belongs to the plugin.
constexpr std::size_t   kStateHeaderSize = 8;
constexpr std::uint8_t  kStateMagic[4]   = { 'T', 'W', 'C', 'P' };
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

}  // namespace

class twClapPlugin final : public twPlugin {
public:
    static std::unique_ptr<twClapPlugin> create( const std::string &path,
                                                 const std::string &uid );
    ~twClapPlugin() override;

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
    twClapPlugin() = default;

    bool init( const std::string &path, const std::string &uid );
    void deactivate();                     // hostMutex_ must be held
    void readPortLayout();
    void readParams();
    void refreshMirror();                  // main thread

    // --- host vtable (static thunks; host_data is the twClapPlugin) ---------
    static const void *hostGetExtension( const clap_host_t *, const char * );
    static void        hostRequestRestart( const clap_host_t * );
    static void        hostRequestProcess( const clap_host_t * );
    static void        hostRequestCallback( const clap_host_t * );

    // --- event plumbing ----------------------------------------------------
    struct ParamEdit {
        std::uint32_t id;
        double        value;
    };
    static constexpr std::uint32_t kRingCapacity = 256;  // power of two

    void pushEdit( std::uint32_t id, double v );
    // Move the ring (and, if flagged, the whole mirror) into events_.
    // Allocation-free: events_ is reserved in prepare().
    void drainEditsIntoEvents();

    static std::uint32_t              eventsSize( const clap_input_events_t * );
    static const clap_event_header_t *eventsGet( const clap_input_events_t *,
                                                 std::uint32_t );
    static bool outEventsTryPush( const clap_output_events_t *,
                                  const clap_event_header_t * );

    struct PortBufs {
        std::vector<std::vector<float>> owned;   // scratch for non-main ports
        std::vector<float *>            ptrs;
    };

    std::shared_ptr<twClapModule> module_;
    const clap_plugin_t          *plugin_ = nullptr;
    clap_host_t                   host_{};

    const clap_plugin_audio_ports_t *extAudioPorts_ = nullptr;
    const clap_plugin_params_t      *extParams_     = nullptr;
    const clap_plugin_state_t       *extState_      = nullptr;
    const clap_plugin_latency_t     *extLatency_    = nullptr;

    twPluginIoLayout io_{};
    std::string      uid_, path_;
    bool             hasGui_       = false;
    bool             acceptsNotes_ = false;

    // Port shape (all ports, not just main — CLAP's process() expects one buffer
    // per declared port; short-changing it is a spec violation, so the non-main
    // ports get zeroed input scratch and throwaway output scratch).
    std::vector<std::uint32_t> inPortChans_, outPortChans_;
    int                        mainInPort_ = -1, mainOutPort_ = -1;

    std::vector<clap_audio_buffer_t> inBufs_, outBufs_;
    std::vector<PortBufs>            inScratch_, outScratch_;
    std::vector<float *>             mainInPtrs_, mainOutPtrs_;

    std::vector<twPluginParamInfo> params_;
    std::vector<clap_id>           paramIds_;
    // Mirror of the plugin's parameter values, so getParam() (UI thread) never
    // calls the plugin. Fixed size after init(), hence a raw array of atomics.
    std::unique_ptr<std::atomic<double>[]> mirror_;

    ParamEdit                  ring_[kRingCapacity];
    std::atomic<std::uint32_t> ringWrite_{ 0 }, ringRead_{ 0 };
    std::atomic<bool>          resyncAll_{ false };

    std::vector<clap_event_param_value_t> events_;
    std::vector<const clap_event_header_t *> eventPtrs_;
    clap_input_events_t                  inEvents_{};
    clap_output_events_t                 outEvents_{};

    // hostMutex_ serializes the activation TRANSITIONS (prepare/deactivate/the
    // inactive flush in setParam). process() never takes it — it only reads the
    // flags, which is why they are atomic rather than plain bools.
    mutable std::mutex        hostMutex_;
    std::atomic<bool>         active_{ false };
    std::atomic<bool>         processing_{ false };
    std::atomic<bool>         processFailed_{ false };
    std::atomic<std::uint32_t> preparedMax_{ 0 };
    std::uint32_t             preparedRate_ = 0;   // hostMutex_ only

    // Host callback requests, recorded only. Nothing here calls back into the
    // graph: the audio/worker side must never reach into component wiring, and
    // twComponent is not a QObject, so there is no signal to emit either.
    std::atomic<bool> restartRequested_{ false };
    std::atomic<bool> processRequested_{ false };
    std::atomic<bool> callbackRequested_{ false };
};

// --- host vtable ------------------------------------------------------------

const void *twClapPlugin::hostGetExtension( const clap_host_t *, const char * )
{
    // M1 hosts no host extensions. Returning nullptr for everything is legal and
    // makes the plugin fall back to polling/no-op behaviour.
    return nullptr;
}

void twClapPlugin::hostRequestRestart( const clap_host_t *h )
{
    if( h && h->host_data )
        ( (twClapPlugin *)h->host_data )->restartRequested_.store( true, std::memory_order_release );
}

void twClapPlugin::hostRequestProcess( const clap_host_t *h )
{
    if( h && h->host_data )
        ( (twClapPlugin *)h->host_data )->processRequested_.store( true, std::memory_order_release );
}

void twClapPlugin::hostRequestCallback( const clap_host_t *h )
{
    if( h && h->host_data )
        ( (twClapPlugin *)h->host_data )->callbackRequested_.store( true, std::memory_order_release );
}

// --- event lists ------------------------------------------------------------

std::uint32_t twClapPlugin::eventsSize( const clap_input_events_t *list )
{
    const twClapPlugin *self = (const twClapPlugin *)list->ctx;
    return (std::uint32_t)self->eventPtrs_.size();
}

const clap_event_header_t *twClapPlugin::eventsGet( const clap_input_events_t *list,
                                                    std::uint32_t              index )
{
    const twClapPlugin *self = (const twClapPlugin *)list->ctx;
    if( index >= self->eventPtrs_.size() )
        return nullptr;
    return self->eventPtrs_[index];
}

bool twClapPlugin::outEventsTryPush( const clap_output_events_t *,
                                     const clap_event_header_t * )
{
    // Output events (the plugin telling us a parameter moved, gestures, notes)
    // are dropped in M1. Returning true keeps well-behaved plugins from
    // retrying/logging; M5's SSetPluginParamAction is where they get a home.
    return true;
}

// --- construction -----------------------------------------------------------

std::unique_ptr<twClapPlugin> twClapPlugin::create( const std::string &path,
                                                    const std::string &uid )
{
    std::unique_ptr<twClapPlugin> p( new twClapPlugin() );
    if( !p->init( path, uid ) )
        return nullptr;
    return p;
}

bool twClapPlugin::init( const std::string &path, const std::string &uid )
{
    path_   = path;
    module_ = twClapModule::open( path );
    if( !module_ )
        return false;

    const clap_plugin_factory_t *f = module_->factory();

    std::string wanted = uid;
    if( wanted.empty() ) {
        if( f->get_plugin_count( f ) == 0 ) {
            TW_LOGE( "plugins", "[clap] '%s' exposes no plugins", path.c_str() );
            return false;
        }
        const clap_plugin_descriptor_t *cd = f->get_plugin_descriptor( f, 0 );
        if( !cd || !cd->id ) {
            TW_LOGE( "plugins", "[clap] '%s' plugin 0 has no id", path.c_str() );
            return false;
        }
        wanted = cd->id;
    }
    uid_ = wanted;

    host_.clap_version = CLAP_VERSION;
    host_.host_data    = this;
    host_.name         = "Smaragd";
    host_.vendor       = "Smaragd";
    host_.url          = "https://github.com/tweggen/qbx";
    host_.version      = "1.0.0";
    host_.get_extension    = &twClapPlugin::hostGetExtension;
    host_.request_restart  = &twClapPlugin::hostRequestRestart;
    host_.request_process  = &twClapPlugin::hostRequestProcess;
    host_.request_callback = &twClapPlugin::hostRequestCallback;

    plugin_ = f->create_plugin( f, &host_, wanted.c_str() );
    if( !plugin_ ) {
        TW_LOGE( "plugins", "[clap] create_plugin('%s') failed in '%s'",
                 wanted.c_str(), path.c_str() );
        return false;
    }
    if( !plugin_->init || !plugin_->init( plugin_ ) ) {
        TW_LOGE( "plugins", "[clap] plugin->init() failed for '%s'", wanted.c_str() );
        if( plugin_->destroy )
            plugin_->destroy( plugin_ );
        plugin_ = nullptr;
        return false;
    }

    if( plugin_->get_extension ) {
        extAudioPorts_ = (const clap_plugin_audio_ports_t *)
            plugin_->get_extension( plugin_, CLAP_EXT_AUDIO_PORTS );
        extParams_ = (const clap_plugin_params_t *)
            plugin_->get_extension( plugin_, CLAP_EXT_PARAMS );
        extState_ = (const clap_plugin_state_t *)
            plugin_->get_extension( plugin_, CLAP_EXT_STATE );
        extLatency_ = (const clap_plugin_latency_t *)
            plugin_->get_extension( plugin_, CLAP_EXT_LATENCY );
        hasGui_ = plugin_->get_extension( plugin_, CLAP_EXT_GUI ) != nullptr;
        acceptsNotes_ =
            plugin_->get_extension( plugin_, CLAP_EXT_NOTE_PORTS ) != nullptr;
    }

    readPortLayout();
    readParams();

    inEvents_.ctx  = this;
    inEvents_.size = &twClapPlugin::eventsSize;
    inEvents_.get  = &twClapPlugin::eventsGet;
    outEvents_.ctx      = this;
    outEvents_.try_push = &twClapPlugin::outEventsTryPush;

    TW_LOGI( "plugins", "[clap] instantiated '%s' from '%s' (%u in / %u out, %u param(s))",
             uid_.c_str(), path.c_str(), (unsigned)io_.audioInputs,
             (unsigned)io_.audioOutputs, (unsigned)params_.size() );
    return true;
}

twClapPlugin::~twClapPlugin()
{
    {
        std::lock_guard<std::mutex> lock( hostMutex_ );
        deactivate();
    }
    if( plugin_ && plugin_->destroy )
        plugin_->destroy( plugin_ );
    plugin_ = nullptr;
    // module_ released last: the DSO must outlive every plugin it created.
}

void twClapPlugin::readPortLayout()
{
    if( !extAudioPorts_ || !extAudioPorts_->count || !extAudioPorts_->get ) {
        // No clap.audio-ports: CLAP's default is no audio at all. Treat it as a
        // stereo insert anyway would be a lie; report 0/0 and let the caller's
        // channel-mismatch policy (M3) decide.
        io_ = twPluginIoLayout{ 0, 0 };
        return;
    }

    for( int dir = 0; dir < 2; ++dir ) {
        const bool isInput = ( dir == 0 );
        std::vector<std::uint32_t> &chans = isInput ? inPortChans_ : outPortChans_;
        int &mainPort = isInput ? mainInPort_ : mainOutPort_;

        const std::uint32_t n = extAudioPorts_->count( plugin_, isInput );
        for( std::uint32_t i = 0; i < n; ++i ) {
            clap_audio_port_info_t info;
            std::memset( &info, 0, sizeof( info ) );
            if( !extAudioPorts_->get( plugin_, i, isInput, &info ) ) {
                chans.push_back( 0 );
                continue;
            }
            chans.push_back( info.channel_count );
            if( mainPort < 0 && ( info.flags & CLAP_AUDIO_PORT_IS_MAIN ) )
                mainPort = (int)i;
        }
        if( mainPort < 0 && !chans.empty() )
            mainPort = 0;   // no IS_MAIN flag anywhere: port 0 is the main port
    }

    io_.audioInputs = (std::uint16_t)( mainInPort_ >= 0 ? inPortChans_[(size_t)mainInPort_] : 0 );
    io_.audioOutputs =
        (std::uint16_t)( mainOutPort_ >= 0 ? outPortChans_[(size_t)mainOutPort_] : 0 );
}

void twClapPlugin::readParams()
{
    if( !extParams_ || !extParams_->count || !extParams_->get_info )
        return;

    const std::uint32_t n = extParams_->count( plugin_ );
    params_.reserve( n );
    paramIds_.reserve( n );

    for( std::uint32_t i = 0; i < n; ++i ) {
        clap_param_info_t ci;
        std::memset( &ci, 0, sizeof( ci ) );
        if( !extParams_->get_info( plugin_, i, &ci ) )
            continue;

        twPluginParamInfo pi;
        pi.id           = (std::uint32_t)ci.id;
        pi.name         = ci.name;   // fixed-size, NUL-terminated by CLAP
        pi.minValue     = ci.min_value;
        pi.maxValue     = ci.max_value;
        pi.defaultValue = ci.default_value;
        pi.isStepped    = ( ci.flags & CLAP_PARAM_IS_STEPPED ) != 0;

        params_.push_back( std::move( pi ) );
        paramIds_.push_back( ci.id );
    }

    mirror_.reset( new std::atomic<double>[ params_.empty() ? 1 : params_.size() ] );
    refreshMirror();
}

void twClapPlugin::refreshMirror()
{
    for( std::size_t i = 0; i < params_.size(); ++i ) {
        double v = params_[i].defaultValue;
        if( extParams_ && extParams_->get_value )
            extParams_->get_value( plugin_, paramIds_[i], &v );
        mirror_[i].store( v, std::memory_order_relaxed );
    }
}

// --- lifecycle --------------------------------------------------------------

void twClapPlugin::prepare( std::uint32_t sampleRate, std::uint32_t maxBlock )
{
    if( !plugin_ )
        return;
    if( maxBlock == 0 )
        maxBlock = 1;

    std::lock_guard<std::mutex> lock( hostMutex_ );

    if( active_.load( std::memory_order_relaxed ) && preparedRate_ == sampleRate &&
        preparedMax_.load( std::memory_order_relaxed ) == maxBlock )
        return;   // idempotent: the steady-state call is a comparison

    deactivate();

    if( !plugin_->activate || !plugin_->activate( plugin_, (double)sampleRate, 1, maxBlock ) ) {
        TW_LOGE( "plugins", "[clap] activate(%u Hz, max %u) failed for '%s'; "
                 "the insert will pass audio through unchanged",
                 (unsigned)sampleRate, (unsigned)maxBlock, uid_.c_str() );
        processFailed_.store( true, std::memory_order_release );
        return;
    }
    preparedRate_ = sampleRate;
    preparedMax_.store( maxBlock, std::memory_order_release );
    processFailed_.store( false, std::memory_order_release );

    // Per-port buffers. Allocated here, never in process().
    auto buildPorts = []( const std::vector<std::uint32_t> &chans, int mainPort,
                          std::uint32_t frames, std::vector<PortBufs> &scratch,
                          std::vector<clap_audio_buffer_t> &bufs ) {
        scratch.assign( chans.size(), PortBufs{} );
        bufs.assign( chans.size(), clap_audio_buffer_t{} );
        for( std::size_t p = 0; p < chans.size(); ++p ) {
            bufs[p].channel_count = chans[p];
            bufs[p].data64        = nullptr;
            bufs[p].latency       = 0;
            bufs[p].constant_mask = 0;
            if( (int)p == mainPort ) {
                bufs[p].data32 = nullptr;   // filled per call from the caller's pointers
                continue;
            }
            scratch[p].owned.assign( chans[p], std::vector<float>( frames, 0.0f ) );
            scratch[p].ptrs.resize( chans[p] );
            for( std::uint32_t c = 0; c < chans[p]; ++c )
                scratch[p].ptrs[c] = scratch[p].owned[c].data();
            bufs[p].data32 = scratch[p].ptrs.empty() ? nullptr : scratch[p].ptrs.data();
        }
    };
    buildPorts( inPortChans_,  mainInPort_,  maxBlock, inScratch_,  inBufs_ );
    buildPorts( outPortChans_, mainOutPort_, maxBlock, outScratch_, outBufs_ );

    mainInPtrs_.assign( io_.audioInputs, nullptr );
    mainOutPtrs_.assign( io_.audioOutputs, nullptr );

    // Worst case per drain: the whole ring plus a full mirror resync.
    events_.reserve( kRingCapacity + params_.size() );
    eventPtrs_.reserve( kRingCapacity + params_.size() );

    if( plugin_->start_processing && plugin_->start_processing( plugin_ ) ) {
        processing_.store( true, std::memory_order_release );
    } else {
        TW_LOGW( "plugins", "[clap] start_processing() failed for '%s'; the insert "
                 "will pass audio through unchanged", uid_.c_str() );
        processing_.store( false, std::memory_order_release );
    }

    // Published last, with release semantics: process() reads active_ without the
    // mutex, so every buffer it will touch must already be built and visible.
    active_.store( true, std::memory_order_release );
}

// hostMutex_ must be held.
void twClapPlugin::deactivate()
{
    if( !plugin_ )
        return;
    const bool wasActive = active_.exchange( false, std::memory_order_acq_rel );
    if( processing_.exchange( false, std::memory_order_acq_rel ) && plugin_->stop_processing )
        plugin_->stop_processing( plugin_ );
    if( wasActive && plugin_->deactivate )
        plugin_->deactivate( plugin_ );
}

void twClapPlugin::reset()
{
    if( plugin_ && plugin_->reset )
        plugin_->reset( plugin_ );
}

std::uint32_t twClapPlugin::reportedLatency() const
{
    if( extLatency_ && extLatency_->get )
        return extLatency_->get( plugin_ );
    return 0;
}

// --- parameters -------------------------------------------------------------

twPluginParamInfo twClapPlugin::paramInfo( std::size_t i ) const
{
    if( i < params_.size() )
        return params_[i];
    return twPluginParamInfo{};
}

double twClapPlugin::getParam( std::uint32_t id ) const
{
    for( std::size_t i = 0; i < params_.size(); ++i )
        if( params_[i].id == id )
            return mirror_[i].load( std::memory_order_acquire );
    return 0.0;
}

std::string twClapPlugin::paramValueText( std::uint32_t id, double v ) const
{
    // CLAP marks params.value_to_text [main-thread]; the Qt UI thread is the host
    // main thread, so this is spec-compliant against a concurrent [audio-thread]
    // process(). No host lock — like getParam(), this must not contend with
    // prepare()/setParam's flush path.
    if( !extParams_ || !extParams_->value_to_text )
        return {};
    char buf[256];
    buf[0] = '\0';
    if( !extParams_->value_to_text( plugin_, id, v, buf, sizeof( buf ) ) )
        return {};
    return std::string( buf );   // CLAP NUL-terminates within cap
}

void twClapPlugin::setParam( std::uint32_t id, double v )
{
    std::size_t idx = params_.size();
    for( std::size_t i = 0; i < params_.size(); ++i ) {
        if( params_[i].id == id ) {
            idx = i;
            break;
        }
    }
    if( idx == params_.size() )
        return;

    const twPluginParamInfo &pi = params_[idx];
    if( v < pi.minValue ) v = pi.minValue;
    if( v > pi.maxValue ) v = pi.maxValue;

    mirror_[idx].store( v, std::memory_order_release );
    pushEdit( id, v );

    // Not active => no render can be in flight => flush() belongs to this thread
    // (CLAP: "[active ? audio-thread : main-thread]"). Taking hostMutex_ is what
    // makes the !active_ observation trustworthy against a concurrent prepare().
    std::lock_guard<std::mutex> lock( hostMutex_ );
    if( !active_.load( std::memory_order_acquire ) && extParams_ && extParams_->flush ) {
        drainEditsIntoEvents();
        if( !eventPtrs_.empty() )
            extParams_->flush( plugin_, &inEvents_, &outEvents_ );
    }
}

void twClapPlugin::pushEdit( std::uint32_t id, double v )
{
    const std::uint32_t w = ringWrite_.load( std::memory_order_relaxed );
    const std::uint32_t r = ringRead_.load( std::memory_order_acquire );
    if( w - r >= kRingCapacity ) {
        // Backlog. Do not drop silently and do not touch ringRead_ from the
        // producer side: raise the resync flag instead, and the next drain
        // re-sends every parameter from the mirror. Last-value-wins makes that
        // strictly better than replaying the backlog.
        resyncAll_.store( true, std::memory_order_release );
        return;
    }
    ring_[w & ( kRingCapacity - 1 )] = ParamEdit{ id, v };
    ringWrite_.store( w + 1, std::memory_order_release );
}

void twClapPlugin::drainEditsIntoEvents()
{
    events_.clear();
    eventPtrs_.clear();

    auto emit = []( std::vector<clap_event_param_value_t> &dst, std::uint32_t id, double v ) {
        clap_event_param_value_t e;
        std::memset( &e, 0, sizeof( e ) );
        e.header.size     = sizeof( clap_event_param_value_t );
        e.header.time     = 0;
        e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        e.header.type     = CLAP_EVENT_PARAM_VALUE;
        e.header.flags    = 0;
        e.param_id        = (clap_id)id;
        e.cookie          = nullptr;
        e.note_id         = -1;
        e.port_index      = -1;
        e.channel         = -1;
        e.key             = -1;
        e.value           = v;
        dst.push_back( e );
    };

    const std::uint32_t w = ringWrite_.load( std::memory_order_acquire );
    std::uint32_t       r = ringRead_.load( std::memory_order_relaxed );

    if( resyncAll_.exchange( false, std::memory_order_acq_rel ) ) {
        // Everything the ring still holds is subsumed by the mirror.
        ringRead_.store( w, std::memory_order_release );
        for( std::size_t i = 0; i < params_.size(); ++i )
            emit( events_, params_[i].id, mirror_[i].load( std::memory_order_acquire ) );
    } else {
        for( ; r != w; ++r ) {
            const ParamEdit &e = ring_[r & ( kRingCapacity - 1 )];
            emit( events_, e.id, e.value );
        }
        ringRead_.store( w, std::memory_order_release );
    }

    // Pointers are stable: events_ was reserved in prepare() and never grows
    // past kRingCapacity + params_.size() entries.
    eventPtrs_.resize( events_.size() );
    for( std::size_t i = 0; i < events_.size(); ++i )
        eventPtrs_[i] = &events_[i].header;
}

// --- processing -------------------------------------------------------------

void twClapPlugin::process( const float *const *in, float *const *out,
                            std::uint32_t nframes )
{
    const std::uint32_t nIn  = io_.audioInputs;
    const std::uint32_t nOut = io_.audioOutputs;

    auto passThrough = [&]() {
        for( std::uint32_t c = 0; c < nOut; ++c ) {
            if( !out[c] )
                continue;
            if( c < nIn && in && in[c] )
                std::memcpy( out[c], in[c], (std::size_t)nframes * sizeof( float ) );
            else
                std::memset( out[c], 0, (std::size_t)nframes * sizeof( float ) );
        }
    };

    if( !plugin_ || !plugin_->process || nframes == 0 ||
        processFailed_.load( std::memory_order_acquire ) ||
        !active_.load( std::memory_order_acquire ) ||
        !processing_.load( std::memory_order_acquire ) ) {
        passThrough();
        return;
    }
    const std::uint32_t maxBlock = preparedMax_.load( std::memory_order_acquire );
    if( nframes > maxBlock ) {
        // The caller must chunk to the value it passed to prepare(); an overlong
        // block would overrun the plugin's own activation-sized buffers.
        TW_LOGE( "plugins", "[clap] process(%u) exceeds prepared max %u for '%s'; "
                 "passing audio through", (unsigned)nframes, (unsigned)maxBlock,
                 uid_.c_str() );
        passThrough();
        return;
    }

    drainEditsIntoEvents();

    // Point the main ports at the caller's de-interleaved buffers. CLAP wants
    // float** for inputs too; the buffers are the host's own scratch (see
    // twPluginInsert), so a plugin that writes them harms nothing.
    for( std::uint32_t c = 0; c < nIn; ++c )
        mainInPtrs_[c] = const_cast<float *>( in ? in[c] : nullptr );
    for( std::uint32_t c = 0; c < nOut; ++c )
        mainOutPtrs_[c] = out ? out[c] : nullptr;

    if( mainInPort_ >= 0 && (std::size_t)mainInPort_ < inBufs_.size() )
        inBufs_[(std::size_t)mainInPort_].data32 =
            mainInPtrs_.empty() ? nullptr : mainInPtrs_.data();
    if( mainOutPort_ >= 0 && (std::size_t)mainOutPort_ < outBufs_.size() )
        outBufs_[(std::size_t)mainOutPort_].data32 =
            mainOutPtrs_.empty() ? nullptr : mainOutPtrs_.data();

    clap_process_t p;
    std::memset( &p, 0, sizeof( p ) );
    p.steady_time         = -1;      // we do not maintain a global sample clock
    p.frames_count        = nframes;
    p.transport           = nullptr; // free-running; no tempo map yet
    p.audio_inputs        = inBufs_.empty() ? nullptr : inBufs_.data();
    p.audio_outputs       = outBufs_.empty() ? nullptr : outBufs_.data();
    p.audio_inputs_count  = (std::uint32_t)inBufs_.size();
    p.audio_outputs_count = (std::uint32_t)outBufs_.size();
    p.in_events           = &inEvents_;
    p.out_events          = &outEvents_;

    const clap_process_status st = plugin_->process( plugin_, &p );
    if( st == CLAP_PROCESS_ERROR ) {
        TW_LOGE( "plugins", "[clap] '%s' returned CLAP_PROCESS_ERROR; "
                 "disabling processing for this instance", uid_.c_str() );
        processFailed_.store( true, std::memory_order_release );
        passThrough();
    }
}

// --- state ------------------------------------------------------------------

std::vector<std::uint8_t> twClapPlugin::saveState() const
{
    std::vector<std::uint8_t> blob( kStateHeaderSize, 0 );
    std::memcpy( blob.data(), kStateMagic, sizeof( kStateMagic ) );
    putU16le( blob.data() + 4, kStateVersion );
    putU16le( blob.data() + 6, 0 );   // reserved

    if( !plugin_ || !extState_ || !extState_->save )
        return blob;   // header only: a plugin without clap.state has no state

    clap_ostream_t os;
    os.ctx   = &blob;
    os.write = []( const clap_ostream_t *s, const void *buf, std::uint64_t size ) -> std::int64_t {
        std::vector<std::uint8_t> *v = (std::vector<std::uint8_t> *)s->ctx;
        const std::uint8_t        *b = (const std::uint8_t *)buf;
        v->insert( v->end(), b, b + (std::size_t)size );
        return (std::int64_t)size;
    };

    if( !extState_->save( plugin_, &os ) ) {
        TW_LOGW( "plugins", "[clap] state save failed for '%s'; storing an empty chunk",
                 uid_.c_str() );
        blob.resize( kStateHeaderSize );
    }
    return blob;
}

bool twClapPlugin::loadState( const std::vector<std::uint8_t> &blob )
{
    if( blob.size() < kStateHeaderSize )
        return false;
    if( std::memcmp( blob.data(), kStateMagic, sizeof( kStateMagic ) ) != 0 ) {
        TW_LOGW( "plugins", "[clap] state blob for '%s' has a foreign magic", uid_.c_str() );
        return false;
    }
    const std::uint16_t ver = getU16le( blob.data() + 4 );
    if( ver > kStateVersion ) {
        TW_LOGW( "plugins", "[clap] state blob for '%s' is version %u, we understand %u; "
                 "keeping the plugin at its defaults", uid_.c_str(), (unsigned)ver,
                 (unsigned)kStateVersion );
        return false;
    }

    const std::size_t payload = blob.size() - kStateHeaderSize;
    if( payload == 0 )
        return true;   // nothing to restore, but the frame was well-formed
    if( !plugin_ || !extState_ || !extState_->load ) {
        TW_LOGW( "plugins", "[clap] '%s' has no clap.state; %llu stored bytes ignored",
                 uid_.c_str(), (unsigned long long)payload );
        return false;
    }

    struct Cursor {
        const std::uint8_t *data;
        std::size_t         size, pos;
    } cur{ blob.data() + kStateHeaderSize, payload, 0 };

    clap_istream_t is;
    is.ctx  = &cur;
    is.read = []( const clap_istream_t *s, void *buf, std::uint64_t size ) -> std::int64_t {
        Cursor     *c = (Cursor *)s->ctx;
        std::size_t n = (std::size_t)std::min<std::uint64_t>( size, c->size - c->pos );
        if( n )
            std::memcpy( buf, c->data + c->pos, n );
        c->pos += n;
        return (std::int64_t)n;
    };

    if( !extState_->load( plugin_, &is ) ) {
        TW_LOGW( "plugins", "[clap] state load failed for '%s'", uid_.c_str() );
        return false;
    }

    // The plugin's values just changed underneath the mirror.
    refreshMirror();
    return true;
}

// --- factory (referenced by name from twPluginRegistry::instantiate) --------

std::unique_ptr<twPlugin> createClapPlugin( const std::string &path,
                                            const std::string &uid )
{
    return twClapPlugin::create( path, uid );
}

}  // namespace audio
