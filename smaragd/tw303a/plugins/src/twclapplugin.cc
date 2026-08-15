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
//
// EVENTS (proposal 36 P2)
// -----------------------
// process(..., twEventList, twEventOut, twProcessContext) translates the host's
// format-free twEvents into CLAP events, appended AFTER the parameter ring's
// (which are all at time 0), so the combined list stays sorted — CLAP requires
// that. The legacy process() overload calls the same code with an EMPTY list
// and an all-invalid context, so it emits exactly the events it emitted before
// and leaves clap_process::transport nullptr: the pre-36 render path is not
// merely equivalent, it is the same instructions, which is what lets the effect
// goldens be compared byte for byte.
//
// DIALECT. A note port declares which dialects it understands. We pick CLAP
// when it is offered (structured notes, note ids, note expressions) and MIDI 1
// otherwise, and we NEVER send the same note in both — a plugin that supports
// both would then play it twice (plugins/CONTRACT.md invariant 30).

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

// One slot of the outgoing event buffer. CLAP events are POD structs of
// different sizes that all begin with a clap_event_header_t; a union sized to
// the largest of them lets ONE pre-reserved vector hold a mixed sequence, with
// stable addresses for the pointer array we hand over. (A vector of the header
// alone would slice; separate vectors per type would not preserve order.)
union ClapEventSlot {
    clap_event_header_t          header;
    clap_event_note_t            note;
    clap_event_note_expression_t expr;
    clap_event_param_value_t     param;
    clap_event_param_mod_t       mod;
    clap_event_param_gesture_t   gesture;
    clap_event_midi_t            midi;
    clap_event_midi_sysex_t      sysex;
    clap_event_transport_t       transport;
};

// MIDI 1.0 status bytes for the dialect-MIDI path.
constexpr std::uint8_t kMidiNoteOff  = 0x80;
constexpr std::uint8_t kMidiNoteOn   = 0x90;
constexpr std::uint8_t kMidiPolyAT   = 0xA0;
constexpr std::uint8_t kMidiCC       = 0xB0;
constexpr std::uint8_t kMidiProgram  = 0xC0;
constexpr std::uint8_t kMidiChanAT   = 0xD0;
constexpr std::uint8_t kMidiPitchBnd = 0xE0;

std::uint8_t clamp7( double v )
{
    if( v < 0.0 ) v = 0.0;
    if( v > 127.0 ) v = 127.0;
    return (std::uint8_t)( v + 0.5 );
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

    std::uint32_t reportedLatency() const override;
    bool supportsNativeEditor() const override { return hasGui_; }

    twPluginCapabilities capabilities() const override { return caps_; }
    std::size_t          audioOutBusCount() const override { return outPortChans_.size(); }
    twPluginBusInfo      audioOutBus( std::size_t i ) const override;
    std::uint32_t        tailFrames() const override;

private:
    twClapPlugin() = default;

    bool init( const std::string &path, const std::string &uid );
    void deactivate();                     // hostMutex_ must be held
    void readPortLayout();
    void readNotePorts();
    void readParams();
    void refreshMirror();                  // main thread

    // Translate the host's twEvents into events_ / eventPtrs_, appended after
    // whatever drainEditsIntoEvents() already put there.
    void appendHostEvents( const twEventList &events, std::uint32_t nframes );

    // --- host vtable (static thunks; host_data is the twClapPlugin) ---------
    static const void *hostGetExtension( const clap_host_t *, const char * );
    static void        hostRequestRestart( const clap_host_t * );
    static void        hostRequestProcess( const clap_host_t * );
    static void        hostRequestCallback( const clap_host_t * );

    // clap.host-note-ports / clap.host-params / clap.host-tail. All three are
    // "tell the host something changed" surfaces; like the request_* callbacks
    // they only RECORD, because nothing on the audio/worker side may reach into
    // component wiring and twComponent is not a QObject to signal from.
    static std::uint32_t hostNotePortsSupportedDialects( const clap_host_t * );
    static void          hostNotePortsRescan( const clap_host_t *, std::uint32_t );
    static void          hostParamsRescan( const clap_host_t *, clap_param_rescan_flags );
    static void          hostParamsClear( const clap_host_t *, clap_id, clap_param_clear_flags );
    static void          hostParamsRequestFlush( const clap_host_t * );
    static void          hostTailChanged( const clap_host_t * );

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
    const clap_plugin_note_ports_t  *extNotePorts_  = nullptr;
    const clap_plugin_tail_t        *extTail_       = nullptr;

    // Host extensions we vend. Static-lifetime vtables: the plugin may keep the
    // pointer for as long as it lives, so they must not be members that move.
    static const clap_host_note_ports_t s_hostNotePorts;
    static const clap_host_params_t     s_hostParams;
    static const clap_host_tail_t       s_hostTail;

    twPluginIoLayout io_{};
    std::string      uid_, path_;
    bool             hasGui_ = false;

    twPluginCapabilities caps_{};

    // Note ports, and the dialect chosen for each INPUT port. The output ports
    // are only counted (we translate whatever the plugin pushes, in either
    // dialect).
    struct NotePort {
        std::uint32_t supported = 0;
        std::uint32_t chosen    = 0;   // exactly ONE clap_note_dialect bit
    };
    std::vector<NotePort> notePortsIn_;
    std::uint32_t         notePortsOutCount_ = 0;
    // The port every note we send goes to. -1 when the plugin declares none.
    int noteOutTargetPort_ = -1;

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

    std::vector<ClapEventSlot>               events_;
    std::vector<const clap_event_header_t *> eventPtrs_;
    clap_input_events_t                      inEvents_{};
    clap_output_events_t                     outEvents_{};

    // Where outEventsTryPush() delivers, for the duration of ONE process()
    // call. Null on the legacy path, which is why that path still discards.
    twEventOut *curEventsOut_ = nullptr;

    // Filled per call from the twProcessContext, and pointed at by
    // clap_process::transport only when the context actually carries something.
    clap_event_transport_t transport_{};

    // hostMutex_ serializes the activation TRANSITIONS (prepare/deactivate/the
    // inactive flush in setParam). process() never takes it — it only reads the
    // flags, which is why they are atomic rather than plain bools.
    mutable std::mutex        hostMutex_;
    std::atomic<bool>         active_{ false };
    std::atomic<bool>         processing_{ false };
    std::atomic<bool>         processFailed_{ false };
    std::atomic<std::uint32_t> preparedMax_{ 0 };
    std::uint32_t             preparedRate_ = 0;   // hostMutex_ only
    // The same value, readable from process() (which never takes hostMutex_).
    std::atomic<std::uint32_t> preparedRateA_{ 0 };

    // Host callback requests, recorded only. Nothing here calls back into the
    // graph: the audio/worker side must never reach into component wiring, and
    // twComponent is not a QObject, so there is no signal to emit either.
    std::atomic<bool> restartRequested_{ false };
    std::atomic<bool> processRequested_{ false };
    std::atomic<bool> callbackRequested_{ false };
};

// --- host vtable ------------------------------------------------------------

const clap_host_note_ports_t twClapPlugin::s_hostNotePorts = {
    &twClapPlugin::hostNotePortsSupportedDialects,
    &twClapPlugin::hostNotePortsRescan,
};

const clap_host_params_t twClapPlugin::s_hostParams = {
    &twClapPlugin::hostParamsRescan,
    &twClapPlugin::hostParamsClear,
    &twClapPlugin::hostParamsRequestFlush,
};

const clap_host_tail_t twClapPlugin::s_hostTail = {
    &twClapPlugin::hostTailChanged,
};

const void *twClapPlugin::hostGetExtension( const clap_host_t *, const char *id )
{
    // Only what we genuinely implement (proposal 36 §5.2). Claiming an
    // extension we answer with nothing useful is how a plugin ends up in a
    // state the host never leaves — e.g. a plugin that sees clap.host-params
    // and stops polling.
    if( !id )
        return nullptr;
    if( std::strcmp( id, CLAP_EXT_NOTE_PORTS ) == 0 )
        return &s_hostNotePorts;
    if( std::strcmp( id, CLAP_EXT_PARAMS ) == 0 )
        return &s_hostParams;
    if( std::strcmp( id, CLAP_EXT_TAIL ) == 0 )
        return &s_hostTail;
    return nullptr;
}

std::uint32_t twClapPlugin::hostNotePortsSupportedDialects( const clap_host_t * )
{
    // We speak both, and pick per port in readNotePorts(). Saying so is what
    // lets a plugin that prefers CLAP declare a CLAP port at all.
    return CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
}

void twClapPlugin::hostNotePortsRescan( const clap_host_t *h, std::uint32_t )
{
    // Recorded only — re-reading the port layout here would race a render, and
    // the layout is already fixed at instantiation (invariant 16 derives the
    // channel mapping from it once). The flag is what a later restart consumes.
    if( h && h->host_data )
        ( (twClapPlugin *)h->host_data )->restartRequested_.store( true, std::memory_order_release );
}

void twClapPlugin::hostParamsRescan( const clap_host_t *h, clap_param_rescan_flags )
{
    if( h && h->host_data )
        ( (twClapPlugin *)h->host_data )->callbackRequested_.store( true, std::memory_order_release );
}

void twClapPlugin::hostParamsClear( const clap_host_t *, clap_id, clap_param_clear_flags )
{
    // We keep no per-parameter automation state inside the backend, so there is
    // nothing to clear. The app's automation lanes are proposal 36 P5.
}

void twClapPlugin::hostParamsRequestFlush( const clap_host_t *h )
{
    // "Please call params.flush() soon." setParam() already flushes when the
    // plugin is inactive and queues an event when it is active, so the honest
    // response is to note it; a flush from THIS thread could race process().
    if( h && h->host_data )
        ( (twClapPlugin *)h->host_data )->callbackRequested_.store( true, std::memory_order_release );
}

void twClapPlugin::hostTailChanged( const clap_host_t * )
{
    // tailFrames() asks the plugin every time, so a changed tail needs no
    // cache invalidation here.
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

bool twClapPlugin::outEventsTryPush( const clap_output_events_t *list,
                                     const clap_event_header_t  *h )
{
    // Proposal 36 P2: the plugin's own events reach the host's twEventOut when
    // one is supplied (the event-aware process() overload). The legacy overload
    // supplies none, so it still DISCARDS — deliberately, because that path
    // must keep producing exactly what it produced before.
    //
    // Returning true either way keeps well-behaved plugins from retrying or
    // logging; the host's own drop counter (twEventOut::dropped) is the honest
    // record of a sink that was too small.
    if( !list || !h )
        return true;
    twClapPlugin *self = (twClapPlugin *)list->ctx;
    if( !self || !self->curEventsOut_ )
        return true;
    if( h->space_id != CLAP_CORE_EVENT_SPACE_ID )
        return true;

    twEvent e;
    e.time = (std::int64_t)h->time;

    switch( h->type ) {
    case CLAP_EVENT_NOTE_ON:
    case CLAP_EVENT_NOTE_OFF:
    case CLAP_EVENT_NOTE_CHOKE:
    case CLAP_EVENT_NOTE_END: {
        const clap_event_note_t *n = (const clap_event_note_t *)h;
        e.kind = h->type == CLAP_EVENT_NOTE_ON    ? twEventKind::NoteOn
                 : h->type == CLAP_EVENT_NOTE_OFF ? twEventKind::NoteOff
                 : h->type == CLAP_EVENT_NOTE_CHOKE ? twEventKind::NoteChoke
                                                    : twEventKind::NoteEnd;
        e.port    = n->port_index;
        e.channel = n->channel;
        e.key     = n->key;
        e.noteId  = n->note_id;
        e.value   = n->velocity;
        break;
    }
    case CLAP_EVENT_NOTE_EXPRESSION: {
        const clap_event_note_expression_t *x = (const clap_event_note_expression_t *)h;
        e.kind    = twEventKind::NoteExpression;
        e.port    = x->port_index;
        e.channel = x->channel;
        e.key     = x->key;
        e.noteId  = x->note_id;
        e.paramId = (std::uint32_t)x->expression_id;
        e.value   = x->value;
        break;
    }
    case CLAP_EVENT_PARAM_VALUE: {
        const clap_event_param_value_t *p = (const clap_event_param_value_t *)h;
        e.kind    = twEventKind::ParamValue;
        e.paramId = (std::uint32_t)p->param_id;
        e.value   = p->value;
        e.port    = p->port_index;
        e.channel = p->channel;
        e.key     = p->key;
        e.noteId  = p->note_id;
        break;
    }
    case CLAP_EVENT_PARAM_GESTURE_BEGIN:
    case CLAP_EVENT_PARAM_GESTURE_END: {
        const clap_event_param_gesture_t *g = (const clap_event_param_gesture_t *)h;
        e.kind    = h->type == CLAP_EVENT_PARAM_GESTURE_BEGIN
                        ? twEventKind::ParamGestureBegin
                        : twEventKind::ParamGestureEnd;
        e.paramId = (std::uint32_t)g->param_id;
        break;
    }
    case CLAP_EVENT_MIDI: {
        const clap_event_midi_t *m = (const clap_event_midi_t *)h;
        e.kind  = twEventKind::Midi1;
        e.port  = (std::int16_t)m->port_index;
        e.value = (double)m->data[0];
        // The remaining two data bytes ride in value2 as a 14-bit pair; a
        // consumer that cares re-splits them. Midi1 is a raw passthrough kind.
        e.value2 = (double)( (std::uint32_t)m->data[1] | ( (std::uint32_t)m->data[2] << 8 ) );
        break;
    }
    default:
        // Everything else (param mod, midi2, sysex out, transport) has no
        // consumer yet; dropping it is not a loss of information the host could
        // act on. Counting it would be misleading — nothing asked for it.
        return true;
    }

    self->curEventsOut_->push( e );
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
        extNotePorts_ = (const clap_plugin_note_ports_t *)
            plugin_->get_extension( plugin_, CLAP_EXT_NOTE_PORTS );
        extTail_ = (const clap_plugin_tail_t *)
            plugin_->get_extension( plugin_, CLAP_EXT_TAIL );
        hasGui_ = plugin_->get_extension( plugin_, CLAP_EXT_GUI ) != nullptr;
    }

    readPortLayout();
    readNotePorts();
    readParams();

    // The descriptor's own feature list is CLAP's category marker; it is the
    // same derivation clapModuleDescriptors() makes for the scanner, made here
    // so an instance answers capabilities() without a registry lookup.
    if( const clap_plugin_descriptor_t *cd = plugin_->desc ) {
        if( cd->features ) {
            for( const char *const *ft = cd->features; *ft; ++ft ) {
                if( std::strcmp( *ft, CLAP_PLUGIN_FEATURE_INSTRUMENT ) == 0 ) {
                    caps_.isInstrument = true;
                    break;
                }
            }
        }
    }
    caps_.emitsParamChanges = extParams_ != nullptr;

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

// clap.note-ports, and the DIALECT decision (proposal 36 §5.2).
//
// A port declares a bitfield of dialects it understands and a preferred one.
// We speak CLAP and MIDI 1; MPE and MIDI 2 are not translated, so a port that
// offers only those gets nothing (and says so once in the log) rather than
// receiving events in a dialect it never asked for.
//
// The choice is per port and it is ONE dialect: sending a note as both a
// clap_event_note and a raw MIDI note-on would make a plugin that understands
// both play it twice.
void twClapPlugin::readNotePorts()
{
    notePortsIn_.clear();
    notePortsOutCount_  = 0;
    noteOutTargetPort_  = -1;
    caps_.acceptsNotes  = false;
    caps_.emitsNotes    = false;
    caps_.notePortsIn   = 0;
    caps_.notePortsOut  = 0;

    if( !extNotePorts_ || !extNotePorts_->count )
        return;

    const std::uint32_t nIn = extNotePorts_->count( plugin_, true );
    for( std::uint32_t i = 0; i < nIn; ++i ) {
        clap_note_port_info_t info;
        std::memset( &info, 0, sizeof( info ) );
        NotePort p;
        if( extNotePorts_->get && extNotePorts_->get( plugin_, i, true, &info ) ) {
            p.supported = info.supported_dialects;
            // Honour the plugin's preference when we speak it; otherwise CLAP
            // first (structured, carries note ids and expressions), then MIDI 1.
            if( ( info.preferred_dialect & ( CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI ) )
                && ( info.supported_dialects & info.preferred_dialect ) )
                p.chosen = info.preferred_dialect
                           & ( CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI );
            else if( info.supported_dialects & CLAP_NOTE_DIALECT_CLAP )
                p.chosen = CLAP_NOTE_DIALECT_CLAP;
            else if( info.supported_dialects & CLAP_NOTE_DIALECT_MIDI )
                p.chosen = CLAP_NOTE_DIALECT_MIDI;
        }
        if( p.chosen == 0 && p.supported != 0 )
            TW_LOGW( "plugins", "[clap] '%s' note input port %u speaks no dialect we "
                     "translate (0x%x); it will receive no events",
                     uid_.c_str(), (unsigned)i, (unsigned)p.supported );
        if( p.chosen != 0 && noteOutTargetPort_ < 0 )
            noteOutTargetPort_ = (int)i;
        notePortsIn_.push_back( p );
    }

    notePortsOutCount_ = extNotePorts_->count( plugin_, false );

    caps_.notePortsIn  = (std::uint16_t)notePortsIn_.size();
    caps_.notePortsOut = (std::uint16_t)notePortsOutCount_;
    caps_.acceptsNotes = !notePortsIn_.empty();
    caps_.emitsNotes   = notePortsOutCount_ > 0;
    if( noteOutTargetPort_ >= 0 ) {
        const std::uint32_t d = notePortsIn_[(std::size_t)noteOutTargetPort_].chosen;
        caps_.wantsMidi1Raw          = ( d == CLAP_NOTE_DIALECT_MIDI );
        caps_.supportsNoteIds        = ( d == CLAP_NOTE_DIALECT_CLAP );
        caps_.supportsNoteExpression = ( d == CLAP_NOTE_DIALECT_CLAP );
    }
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
    preparedRateA_.store( sampleRate, std::memory_order_release );
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

    // Worst case per call: the whole ring plus a full mirror resync, plus a
    // full host event list. Reserved HERE and never grown in process()
    // (CONTRACT invariant 2) — and the reserve is what makes the addresses in
    // eventPtrs_ stable for the duration of the call.
    const std::size_t worstCase =
        kRingCapacity + params_.size() + twEventLimits::kMaxEventsPerBlock;
    events_.reserve( worstCase );
    eventPtrs_.reserve( worstCase );

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

std::uint32_t twClapPlugin::tailFrames() const
{
    if( !extTail_ || !extTail_->get )
        return 0;
    // CLAP reports UINT32_MAX for "infinite" (a feedback delay). Passing that
    // up would make the pre-roll and the project end arithmetic overflow, so it
    // is clamped to something a caller can budget for: 10 seconds at 48 kHz.
    const std::uint32_t t = extTail_->get( plugin_ );
    constexpr std::uint32_t kInfiniteCap = 480000;
    return t > kInfiniteCap ? kInfiniteCap : t;
}

twPluginBusInfo twClapPlugin::audioOutBus( std::size_t i ) const
{
    twPluginBusInfo b;
    if( i >= outPortChans_.size() )
        return b;
    b.channels = (std::uint16_t)outPortChans_[i];
    b.isMain   = ( (int)i == mainOutPort_ );
    return b;
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

    auto emit = []( std::vector<ClapEventSlot> &dst, std::uint32_t id, double v ) {
        ClapEventSlot s;
        std::memset( &s, 0, sizeof( s ) );
        clap_event_param_value_t &e = s.param;
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
        dst.push_back( s );
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

    // Pointers are filled after appendHostEvents(), once events_ has stopped
    // growing: taking them here would dangle if the vector ever reallocated.
    // (It cannot — prepare() reserved the worst case — but the ordering costs
    // nothing and does not depend on that promise.)
    eventPtrs_.resize( events_.size() );
    for( std::size_t i = 0; i < events_.size(); ++i )
        eventPtrs_[i] = &events_[i].header;
}

// Translate the host's twEvents and APPEND them to events_ (proposal 36 §5.2).
//
// Ordering: everything drainEditsIntoEvents() produced is at time 0 and the
// host list is sorted with times >= 0, so appending keeps the whole list sorted
// — which CLAP requires and which a plugin is entitled to rely on.
void twClapPlugin::appendHostEvents( const twEventList &list, std::uint32_t nframes )
{
    if( list.count == 0 )
        return;

    const int  port    = noteOutTargetPort_;
    const bool haveCl  = port >= 0
                        && notePortsIn_[(std::size_t)port].chosen == CLAP_NOTE_DIALECT_CLAP;
    const bool haveMid = port >= 0
                        && notePortsIn_[(std::size_t)port].chosen == CLAP_NOTE_DIALECT_MIDI;
    const std::uint16_t portIdx = port >= 0 ? (std::uint16_t)port : 0;

    std::uint32_t taken = 0;
    for( std::uint32_t i = 0; i < list.count; ++i ) {
        if( taken >= twEventLimits::kMaxEventsPerBlock )
            break;   // the host over-filled its own list; the reserve is the cap
        const twEvent &ev = list.events[i];

        // Metadata never reaches a plugin (twevent.h). A host that leaks one is
        // a bug, but dropping it here is strictly better than forwarding a
        // Tempo event as an Unknown CLAP type.
        if( twEventIsMetadata( ev.kind ) )
            continue;

        std::uint32_t t = ev.time < 0 ? 0u : (std::uint32_t)ev.time;
        if( nframes > 0 && t >= nframes )
            t = nframes - 1;   // clamp, never drop: a late event still belongs

        ClapEventSlot s;
        std::memset( &s, 0, sizeof( s ) );
        s.header.time     = t;
        s.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        s.header.flags    = 0;

        auto asNote = [&]( std::uint16_t type ) {
            s.header.size     = sizeof( clap_event_note_t );
            s.header.type     = type;
            s.note.note_id    = ev.noteId;
            s.note.port_index = (std::int16_t)portIdx;
            s.note.channel    = ev.channel;
            s.note.key        = ev.key;
            s.note.velocity   = ev.value;
        };
        auto asMidi = [&]( std::uint8_t status, std::uint8_t d1, std::uint8_t d2 ) {
            s.header.size     = sizeof( clap_event_midi_t );
            s.header.type     = CLAP_EVENT_MIDI;
            s.midi.port_index = portIdx;
            s.midi.data[0]    = (std::uint8_t)( status
                                    | (std::uint8_t)( ev.channel >= 0 ? ( ev.channel & 0x0F ) : 0 ) );
            s.midi.data[1]    = d1;
            s.midi.data[2]    = d2;
        };

        switch( ev.kind ) {
        case twEventKind::NoteOn:
            if( haveCl )       asNote( CLAP_EVENT_NOTE_ON );
            else if( haveMid ) asMidi( kMidiNoteOn, clamp7( (double)ev.key ),
                                       clamp7( ev.value * 127.0 ) );
            else               continue;
            break;
        case twEventKind::NoteOff:
            if( haveCl )       asNote( CLAP_EVENT_NOTE_OFF );
            else if( haveMid ) asMidi( kMidiNoteOff, clamp7( (double)ev.key ),
                                       clamp7( ev.value * 127.0 ) );
            else               continue;
            break;
        case twEventKind::NoteChoke:
            if( haveCl ) asNote( CLAP_EVENT_NOTE_CHOKE );
            // MIDI 1 has no choke. An all-notes-off would silence unrelated
            // voices, so the honest translation is none.
            else continue;
            break;
        case twEventKind::NoteEnd:
            // Plugin -> host only; a host must never send it.
            continue;
        case twEventKind::NoteExpression:
            if( !haveCl )
                continue;   // no MIDI 1 equivalent that is not a lie
            s.header.size        = sizeof( clap_event_note_expression_t );
            s.header.type        = CLAP_EVENT_NOTE_EXPRESSION;
            s.expr.expression_id = (clap_note_expression)ev.paramId;
            s.expr.note_id       = ev.noteId;
            s.expr.port_index    = (std::int16_t)portIdx;
            s.expr.channel       = ev.channel;
            s.expr.key           = ev.key;
            s.expr.value         = ev.value;
            break;
        case twEventKind::PolyPressure:
            if( !haveMid ) continue;
            asMidi( kMidiPolyAT, clamp7( (double)ev.key ), clamp7( ev.value * 127.0 ) );
            break;
        case twEventKind::ControlChange:
            if( !haveMid ) continue;
            asMidi( kMidiCC, clamp7( (double)ev.paramId ), clamp7( ev.value * 127.0 ) );
            break;
        case twEventKind::PitchBend: {
            if( !haveMid ) continue;
            // value is -1..+1; MIDI 1 bend is 14 bits with 8192 at centre.
            double b = ev.value;
            if( b < -1.0 ) b = -1.0;
            if( b > 1.0 ) b = 1.0;
            const int raw = (int)( 8192.0 + b * 8191.0 + 0.5 );
            asMidi( kMidiPitchBnd, (std::uint8_t)( raw & 0x7F ),
                    (std::uint8_t)( ( raw >> 7 ) & 0x7F ) );
            break;
        }
        case twEventKind::ChannelPressure:
            if( !haveMid ) continue;
            asMidi( kMidiChanAT, clamp7( ev.value * 127.0 ), 0 );
            break;
        case twEventKind::ProgramChange:
            if( !haveMid ) continue;
            asMidi( kMidiProgram, clamp7( ev.value ), 0 );
            break;
        case twEventKind::Midi1:
            if( !haveMid ) continue;
            asMidi( (std::uint8_t)( (int)ev.value & 0xF0 ),
                    (std::uint8_t)( (int)ev.value2 & 0x7F ),
                    (std::uint8_t)( ( (int)ev.value2 >> 8 ) & 0x7F ) );
            // A raw MIDI1 event carries its OWN status byte including the
            // channel; asMidi() would have OR'd ours in on top of it.
            s.midi.data[0] = (std::uint8_t)( (int)ev.value & 0xFF );
            break;
        case twEventKind::Sysex: {
            const std::uint8_t *bytes = list.payloadOf( ev );
            if( !haveMid || !bytes )
                continue;
            s.header.size      = sizeof( clap_event_midi_sysex_t );
            s.header.type      = CLAP_EVENT_MIDI_SYSEX;
            s.sysex.port_index = portIdx;
            // The host arena outlives the call, which is exactly the lifetime
            // CLAP promises for a sysex buffer — no copy needed.
            s.sysex.buffer = bytes;
            s.sysex.size   = ev.payloadSize;
            break;
        }
        case twEventKind::ParamValue:
            s.header.size     = sizeof( clap_event_param_value_t );
            s.header.type     = CLAP_EVENT_PARAM_VALUE;
            s.param.param_id  = (clap_id)ev.paramId;
            s.param.cookie    = nullptr;
            s.param.note_id   = ev.noteId;
            s.param.port_index = -1;
            s.param.channel   = -1;
            s.param.key       = -1;
            s.param.value     = ev.value;
            break;
        case twEventKind::ParamMod:
            s.header.size    = sizeof( clap_event_param_mod_t );
            s.header.type    = CLAP_EVENT_PARAM_MOD;
            s.mod.param_id   = (clap_id)ev.paramId;
            s.mod.cookie     = nullptr;
            s.mod.note_id    = ev.noteId;
            s.mod.port_index = -1;
            s.mod.channel    = -1;
            s.mod.key        = -1;
            s.mod.amount     = ev.value;
            break;
        case twEventKind::ParamGestureBegin:
        case twEventKind::ParamGestureEnd:
            s.header.size      = sizeof( clap_event_param_gesture_t );
            s.header.type      = ev.kind == twEventKind::ParamGestureBegin
                                     ? CLAP_EVENT_PARAM_GESTURE_BEGIN
                                     : CLAP_EVENT_PARAM_GESTURE_END;
            s.gesture.param_id = (clap_id)ev.paramId;
            break;
        case twEventKind::Transport:
            // The transport reaches the plugin through clap_process::transport,
            // built from twProcessContext — a per-call statement, not an event.
            continue;
        default:
            continue;
        }

        events_.push_back( s );
        ++taken;
    }

    // Rebuild the pointer array over the WHOLE list (drainEditsIntoEvents filled
    // it for the ring's share only).
    eventPtrs_.resize( events_.size() );
    for( std::size_t i = 0; i < events_.size(); ++i )
        eventPtrs_[i] = &events_[i].header;
}

// --- processing -------------------------------------------------------------

// The LEGACY overload. It forwards to the event-aware one with an empty list,
// a sink nothing can reach, and an all-invalid context — so the instructions
// executed are identical to the pre-36 ones: no host events are appended,
// clap_process::transport stays nullptr, and outEventsTryPush() still discards
// because curEventsOut_ is null. That identity is what the byte-`cmp` gate on
// the effect goldens rests on.
void twClapPlugin::process( const float *const *in, float *const *out,
                            std::uint32_t nframes )
{
    const twEventList    noEvents{};
    twEventOut           noSink;      // no storage: every push is a counted drop
    const twProcessContext noCtx{};
    float *const *const  outBuses[1] = { out };
    process( in, outBuses, nframes, noEvents, noSink, noCtx );
}

void twClapPlugin::process( const float *const *in, float *const *const *outBuses,
                            std::uint32_t nframes, const twEventList &hostEvents,
                            twEventOut &eventsOut, const twProcessContext &ctx )
{
    // Only the MAIN bus is wired: bus > 0 is aux output, which nothing consumes
    // yet (proposal 36 §5.4). Reading outBuses[0] is what the legacy overload
    // always did.
    float *const *out = ( outBuses && !outPortChans_.empty() ) ? outBuses[0] : nullptr;

    const std::uint32_t nIn  = io_.audioInputs;
    const std::uint32_t nOut = io_.audioOutputs;

    auto passThrough = [&]() {
        if( !out )
            return;
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
    appendHostEvents( hostEvents, nframes );

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

    // Transport (proposal 36 §5.2). Built only from what the context CLAIMS to
    // know: an all-invalid context (which is what the legacy overload passes)
    // leaves p.transport nullptr, exactly as before. steady_time stays -1 —
    // pages are frozen out of order, so a monotonic sample clock would be a
    // lie; the position lives in the transport instead.
    const clap_event_transport_t *transportPtr = nullptr;
    if( ctx.validFlags != twCtxNone ) {
        std::memset( &transport_, 0, sizeof( transport_ ) );
        transport_.header.size     = sizeof( clap_event_transport_t );
        transport_.header.time     = 0;
        transport_.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        transport_.header.type     = CLAP_EVENT_TRANSPORT;
        transport_.flags           = ctx.playing ? CLAP_TRANSPORT_IS_PLAYING : 0;
        if( ctx.has( twCtxTempo ) ) {
            transport_.tempo = ctx.tempoBpm;
            transport_.flags |= CLAP_TRANSPORT_HAS_TEMPO;
        }
        if( ctx.has( twCtxTimeSig ) ) {
            transport_.tsig_num   = (std::uint16_t)ctx.tsNum;
            transport_.tsig_denom = (std::uint16_t)ctx.tsDen;
            transport_.flags |= CLAP_TRANSPORT_HAS_TIME_SIGNATURE;
        }
        if( ctx.has( twCtxPpqPosition ) ) {
            transport_.song_pos_beats =
                (clap_beattime)( ctx.ppqPos * (double)CLAP_BEATTIME_FACTOR );
            transport_.flags |= CLAP_TRANSPORT_HAS_BEATS_TIMELINE;
        }
        if( ctx.has( twCtxPosition ) && preparedRateA_.load( std::memory_order_acquire ) > 0 ) {
            const double secs = (double)ctx.position
                                / (double)preparedRateA_.load( std::memory_order_acquire );
            transport_.song_pos_seconds =
                (clap_sectime)( secs * (double)CLAP_SECTIME_FACTOR );
            transport_.flags |= CLAP_TRANSPORT_HAS_SECONDS_TIMELINE;
        }
        transportPtr = &transport_;
    }

    // Where the plugin's own events land for the duration of this call. Cleared
    // afterwards so a stray push from another thread cannot write into a sink
    // whose storage the host has moved on from.
    curEventsOut_ = &eventsOut;

    clap_process_t p;
    std::memset( &p, 0, sizeof( p ) );
    p.steady_time         = -1;      // pages freeze out of order; see above
    p.frames_count        = nframes;
    p.transport           = transportPtr;
    p.audio_inputs        = inBufs_.empty() ? nullptr : inBufs_.data();
    p.audio_outputs       = outBufs_.empty() ? nullptr : outBufs_.data();
    p.audio_inputs_count  = (std::uint32_t)inBufs_.size();
    p.audio_outputs_count = (std::uint32_t)outBufs_.size();
    p.in_events           = &inEvents_;
    p.out_events          = &outEvents_;

    const clap_process_status st = plugin_->process( plugin_, &p );
    curEventsOut_ = nullptr;
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
