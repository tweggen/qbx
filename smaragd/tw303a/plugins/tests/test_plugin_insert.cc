#include "tw/plugins/twplugininsert.h"
#include "tw/plugins/twpluginslotproc.h"
#include "tw/graph/tw303aenv.h"
#include "tw/pages/io_vector.h"
#include "tw/plugins/twplugindescriptor.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <future>
#include <iostream>
#include <thread>
#include <vector>

namespace audio {

#ifdef TW_TESTVST3_PATH
// Defined in the backend's PRIVATE header (plugins/src/twvst3module.h). Named
// here rather than included, so this test does not drag the VST3 SDK headers —
// and their deliberately PRIVATE include path — into a target that needs exactly
// one symbol from them.
std::vector<twPluginDescriptor> vst3ModuleDescriptors( const std::string &path );
#endif

namespace {

int gFailures = 0;

bool check( bool ok, const char *what )
{
    if( ok ) {
        std::cout << "  ok   " << what << std::endl;
    } else {
        std::cerr << "  FAIL " << what << std::endl;
        ++gFailures;
    }
    return ok;
}

bool nearly( double a, double b, double eps = 1e-6 )
{
    return std::fabs( a - b ) <= eps;
}

// ---------------------------------------------------------------------------
// A twPlugin with a configurable channel layout, so the channel-mismatch table
// of proposal 08 §Layer 3 can be exercised without needing four different real
// plugins. Output channel c is input channel c scaled by gain * (c + 1): the
// per-channel asymmetry is what makes "did bus 1 really get its own wire?"
// observable at all.
class MockPlugin : public twPlugin {
public:
    MockPlugin( int nIn, int nOut, std::atomic<int> *liveCount = nullptr )
        : live_( liveCount )
    {
        io_.audioInputs  = (std::uint16_t)nIn;
        io_.audioOutputs = (std::uint16_t)nOut;
        if( live_ ) live_->fetch_add( 1 );
    }
    ~MockPlugin() override { if( live_ ) live_->fetch_sub( 1 ); }

    const twPluginIoLayout &ioLayout() const override { return io_; }

    void prepare( std::uint32_t sampleRate, std::uint32_t maxBlock ) override
    {
        rate_     = sampleRate;
        maxBlock_ = maxBlock;
        ++prepares_;
    }

    void process( const float *const *in, float *const *out,
                  std::uint32_t nframes ) override
    {
        if( nframes > maxBlock_ ) ++overruns_;
        if( nframes > maxSeen_ )  maxSeen_ = nframes;
        for( std::uint16_t c = 0; c < io_.audioOutputs; ++c ) {
            const float *src = ( c < io_.audioInputs ) ? in[c] : nullptr;
            const float  g   = gain_ * (float)( c + 1 );
            for( std::uint32_t i = 0; i < nframes; ++i )
                out[c][i] = src ? src[i] * g : 0.0f;
        }
    }

    void reset() override { ++resets_; }

    std::size_t       paramCount() const override { return 1; }
    twPluginParamInfo paramInfo( std::size_t ) const override
    {
        return twPluginParamInfo{ 0, "Gain", 0.0, 8.0, 1.0, false };
    }
    double getParam( std::uint32_t ) const override { return gain_; }
    void   setParam( std::uint32_t, double v ) override { gain_ = (float)v; }

    std::vector<std::uint8_t> saveState() const override { return {}; }
    bool loadState( const std::vector<std::uint8_t> & ) override { return true; }

    std::uint32_t maxSeen_  = 0;
    int           overruns_ = 0;
    int           prepares_ = 0;
    int           resets_   = 0;

private:
    twPluginIoLayout  io_{};
    float             gain_     = 1.0f;
    std::uint32_t     rate_     = 0;
    std::uint32_t     maxBlock_ = 0xffffffffu;
    std::atomic<int> *live_     = nullptr;
};

// A position-deterministic mono source: value(p) depends only on the absolute
// frame position, so a page's contents can be predicted exactly and a page
// served for the wrong position is detectable.
class TestSource : public twComponent {
public:
    TestSource( tw303aEnvironment &env, float base )
        : twComponent( env ), base_( base ) {}

    idx_t getNInputs() const override  { return 0; }
    idx_t getNOutputs() const override { return 1; }
    const char *getInputName( idx_t ) const override  { return nullptr; }
    const char *getOutputName( idx_t ) const override { return nullptr; }

    void createOutputLatches() override
    {
        pOutputLatches_.resize( 1 );
        pOutputLatches_[0] =
            std::make_shared<twStreamingLatch>( shared_from_this(), 0, 4096 );
    }

    void reset() override {}
    bool isSeekable() const override { return true; }
    // It carries a cursor, so its freeze must serialize (proposal 19 Phase 1).
    bool usesSerialCursor() const override { return true; }
    int  seekTo( offset_t o ) override { pos_ = o; return 0; }

    length_t renderFrames( sample_t *out, length_t len, const sample_t *,
                           length_t, idx_t ) override
    {
        for( length_t i = 0; i < len; ++i ) out[i] = value( pos_ + i );
        return len;
    }

    float value( offset_t p ) const
    {
        return base_ * (float)( ( p % 17 ) + 1 ) * 0.01f;
    }

private:
    float    base_;
    offset_t pos_ = 0;
};

// Build a slot: one processor + nBuses taps, each fed by its own TestSource.
struct Rig {
    std::shared_ptr<twPluginSlotProcessor>       proc;
    std::vector<std::shared_ptr<TestSource>>     sources;
    std::vector<std::shared_ptr<twPluginInsert>> taps;
};

Rig buildRig( tw303aEnvironment &env, int nBuses, int pluginIn, int pluginOut,
              std::atomic<int> *live = nullptr )
{
    Rig r;
    r.proc = std::make_shared<twPluginSlotProcessor>(
        env,
        [pluginIn, pluginOut, live]() -> std::unique_ptr<twPlugin> {
            return std::make_unique<MockPlugin>( pluginIn, pluginOut, live );
        },
        twPluginIoLayout{ (std::uint16_t)pluginIn, (std::uint16_t)pluginOut } );
    r.proc->setBusCount( (idx_t)nBuses );

    for( int b = 0; b < nBuses; ++b ) {
        auto src = std::make_shared<TestSource>( env, (float)( b + 1 ) );
        src->init();
        auto tap = std::make_shared<twPluginInsert>( env, r.proc, (idx_t)b );
        tap->init();
        tap->setInput( 0, src->linkOutput( 0 ) );
        r.sources.push_back( std::move( src ) );
        r.taps.push_back( std::move( tap ) );
    }
    return r;
}

std::shared_ptr<twOutputPage> freezeTap( const std::shared_ptr<twPluginInsert> &tap,
                                        offset_t pos, int rate )
{
    return tap->requestPage( pos, nullptr, 0,
                             (length_t)twOutputPage::FRAME_CAPACITY, rate, nullptr );
}

}  // namespace

// Phase 1 proof-of-concept, kept as the built-in-plugin smoke test:
// instantiate the PassThrough bit-crusher through the registry and check the
// descriptor / parameter / state surface.
static int testBuiltinPlugin()
{
    std::cout << "=== Built-in plugin (tw.passthrough) ===" << std::endl;

    tw303aEnvironment env;

    auto &registry = pluginRegistry();
    registry.rescan();

    const auto &plugins = registry.plugins();
    if( !check( !plugins.empty(), "registry lists at least one plugin" ) )
        return 1;

    twPluginDescriptor pt;
    if( !check( registry.findByUid( "tw", "tw.passthrough", pt ),
                "registry resolves tw.passthrough by uid" ) )
        return 1;

    std::unique_ptr<twPlugin> plugin = registry.instantiate( pt );
    if( !check( plugin != nullptr, "instantiate PassThrough" ) )
        return 1;

    check( plugin->ioLayout().audioInputs == 2 && plugin->ioLayout().audioOutputs == 2,
           "PassThrough is 2-in / 2-out" );

    auto insert = std::make_unique<twPluginInsert>( env, std::move( plugin ) );

    // Since M3 an insert is a per-bus TAP: always exactly one mono wire in and
    // one out, whatever the plugin's own channel count is. Channel coherence
    // lives in the shared twPluginSlotProcessor, not in the component.
    check( insert->getNInputs() == 1 && insert->getNOutputs() == 1,
           "a tap is 1-in / 1-out regardless of the plugin's layout" );
    check( insert->getPlugin() != nullptr, "the tap exposes its plugin" );
    check( insert->getPlugin()->paramCount() == 1, "PassThrough has 1 parameter" );

    auto state = insert->getPlugin()->saveState();
    check( insert->getPlugin()->loadState( state ), "PassThrough state round-trips" );

    // An unknown format must be refused, not crash: the registry has to survive
    // the descriptors a stale project file or a failed scan will hand it.
    twPluginDescriptor bogus;
    bogus.format = "nope";
    bogus.uid    = "nope.nothing";
    check( registry.instantiate( bogus ) == nullptr,
           "registry refuses an unknown plugin format" );

    return 0;
}

// ---------------------------------------------------------------------------
// M3: the channel-mismatch table of proposal 08 §Layer 3.
static int testChannelPolicy()
{
    std::cout << "=== M3 channel-mismatch policy ===" << std::endl;

    tw303aEnvironment env;
    env.setSRate( 48000 );
    const int rate = env.getSRate();

    // --- N -> N: the normal case ------------------------------------------
    {
        std::atomic<int> live{ 0 };
        Rig r = buildRig( env, 2, 2, 2, &live );
        check( r.proc->mode() == twPluginSlotMode::Direct,
               "2-in/2-out on 2 buses is Direct" );
        check( r.proc->state() == twPluginSlotState::Active, "...and Active" );
        check( live.load() == 1, "...served by exactly ONE plugin instance" );
    }

    // --- 1 -> 1 on N buses: dual-mono -------------------------------------
    {
        std::atomic<int> live{ 0 };
        Rig r = buildRig( env, 2, 1, 1, &live );
        check( r.proc->mode() == twPluginSlotMode::DualMono,
               "1-in/1-out on 2 buses is DualMono" );
        check( live.load() == 2,
               "...served by ONE INSTANCE PER BUS (why the processor takes a factory)" );

        // Prove the instances are genuinely independent: give bus 1's its own
        // gain and watch only bus 1 change.
        std::vector<twPlugin *> ps = r.proc->plugins();
        if( check( ps.size() == 2, "both dual-mono instances are reachable" ) ) {
            ps[1]->setParam( 0, 3.0 );
            r.proc->bumpParamEpoch();
            auto p0 = freezeTap( r.taps[0], 0, rate );
            auto p1 = freezeTap( r.taps[1], 0, rate );
            const float in0 = r.sources[0]->value( 100 );
            const float in1 = r.sources[1]->value( 100 );
            check( p0 && nearly( p0->samples[100], in0 * 1.0f, 1e-6 ),
                   "dual-mono bus 0 keeps its own gain" );
            check( p1 && nearly( p1->samples[100], in1 * 3.0f, 1e-5 ),
                   "dual-mono bus 1 uses its own instance's gain" );
        }
    }

    // --- 2 -> 2 on ONE bus: feed both inputs, average the outputs ---------
    {
        Rig r = buildRig( env, 1, 2, 2 );
        check( r.proc->mode() == twPluginSlotMode::MonoFold,
               "2-in/2-out on 1 bus is MonoFold" );
        auto p = freezeTap( r.taps[0], 0, rate );
        // MockPlugin scales channel c by (c+1), and both inputs see the same
        // mono wire, so the average of the two outputs is in * 1.5.
        const float in = r.sources[0]->value( 77 );
        check( p && nearly( p->samples[77], in * 1.5f, 1e-5 ),
               "MonoFold feeds both inputs and averages the outputs" );
    }

    // --- anything else: Unsupported, transparent, logged once -------------
    {
        Rig r = buildRig( env, 2, 3, 3 );
        check( r.proc->mode() == twPluginSlotMode::Transparent,
               "3-in/3-out on 2 buses has no mapping" );
        check( r.proc->state() == twPluginSlotState::Unsupported,
               "...and the slot is Unsupported" );
        auto p0 = freezeTap( r.taps[0], 0, rate );
        auto p1 = freezeTap( r.taps[1], 0, rate );
        check( p0 && nearly( p0->samples[5], r.sources[0]->value( 5 ), 1e-6 ) &&
                   p1 && nearly( p1->samples[5], r.sources[1]->value( 5 ), 1e-6 ),
               "...and it loads TRANSPARENT (input reaches the output unchanged)" );
    }
    {
        Rig r = buildRig( env, 1, 1, 2 );
        check( r.proc->state() == twPluginSlotState::Unsupported,
               "an asymmetric 1-in/2-out plugin is Unsupported" );
    }

    return 0;
}

// ---------------------------------------------------------------------------
// M4: the missing-plugin placeholder, and becoming Active again after a rescan.
//
// Both halves of what proposal 08 AC 5 promises, at the level the app cannot
// reach: a slot whose factory produces NOTHING must still have the graph shape
// its DESCRIPTOR declared (so installing the plugin later changes only what
// process() computes), and setFactory() must be able to turn it Active in place
// — the taps and the twPluginChains holding them are never rebuilt, because a
// slot's identity in the graph is its processor.
static int testMissingAndReload()
{
    std::cout << "=== M4 missing placeholder + reload ===" << std::endl;

    tw303aEnvironment env;
    env.setSRate( 48000 );
    const int rate = env.getSRate();

    // The factory the app installs: it returns null exactly while the plugin is
    // "not installed", which is what twPluginRegistry::instantiate() does for an
    // unknown uid or a module that is not on disk.
    std::atomic<bool> installed{ false };
    std::atomic<int>  live{ 0 };
    auto factory = [&installed, &live]() -> std::unique_ptr<twPlugin> {
        if( !installed.load() ) return nullptr;
        return std::make_unique<MockPlugin>( 2, 2, &live );
    };

    auto proc = std::make_shared<twPluginSlotProcessor>(
        env, factory, twPluginIoLayout{ 2, 2 } );   // the DECLARED layout
    proc->setBusCount( 2 );

    std::vector<std::shared_ptr<TestSource>>     sources;
    std::vector<std::shared_ptr<twPluginInsert>> taps;
    for( int b = 0; b < 2; ++b ) {
        auto src = std::make_shared<TestSource>( env, (float)( b + 1 ) );
        src->init();
        auto tap = std::make_shared<twPluginInsert>( env, proc, (idx_t)b );
        tap->init();
        tap->setInput( 0, src->linkOutput( 0 ) );
        sources.push_back( std::move( src ) );
        taps.push_back( std::move( tap ) );
    }

    check( proc->state() == twPluginSlotState::Missing,
           "a factory that produces nothing leaves the slot MISSING" );
    check( proc->mode() == twPluginSlotMode::Direct,
           "...but the mapping is still Direct, derived from the DECLARED 2-in/2-out "
           "(so a reload does not change the graph's shape)" );
    check( live.load() == 0, "...and no real plugin instance exists" );
    {
        auto p0 = freezeTap( taps[0], 0, rate );
        auto p1 = freezeTap( taps[1], 0, rate );
        check( p0 && nearly( p0->samples[9], sources[0]->value( 9 ), 1e-6 ) &&
                   p1 && nearly( p1->samples[9], sources[1]->value( 9 ), 1e-6 ),
               "...the placeholder is bit-transparent on every bus" );
    }

    // The rescan found it. Same processor, new factory.
    installed.store( true );
    proc->setFactory( [&installed, &live]() -> std::unique_ptr<twPlugin> {
        if( !installed.load() ) return nullptr;
        return std::make_unique<MockPlugin>( 2, 2, &live );
    } );

    check( proc->state() == twPluginSlotState::Active,
           "setFactory() turns a MISSING slot Active without touching the taps" );
    check( live.load() == 1, "...with exactly one real instance for Direct" );
    {
        // MockPlugin scales channel c by gain * (c + 1) — so bus 1 at 2x is the
        // proof the taps still reach the SAME processor after the swap.
        auto p0 = freezeTap( taps[0], 0, rate );
        auto p1 = freezeTap( taps[1], 0, rate );
        check( p0 && nearly( p0->samples[9], sources[0]->value( 9 ) * 1.0f, 1e-6 ),
               "...bus 0 is now processed" );
        check( p1 && nearly( p1->samples[9], sources[1]->value( 9 ) * 2.0f, 1e-5 ),
               "...bus 1 too, through the same taps as before" );
    }

    // A state chunk re-applied after the reload has to be audible, which is what
    // makes "the settings survived the plugin being missing" true rather than
    // merely stored. (The app does exactly this in SPluginSlot::reloadPlugin.)
    for( twPlugin *p : proc->plugins() ) p->setParam( 0, 4.0 );
    proc->bumpParamEpoch();
    {
        auto p0 = freezeTap( taps[0], 0, rate );
        check( p0 && nearly( p0->samples[9], sources[0]->value( 9 ) * 4.0f, 1e-5 ),
               "...and a parameter applied after the reload is audible" );
    }

    // Going the other way: the plugin disappeared under us (a rescan that lost
    // it). The slot must fall BACK to the placeholder, not keep a dangling
    // instance or go silent.
    installed.store( false );
    proc->setFactory( [&installed, &live]() -> std::unique_ptr<twPlugin> {
        if( !installed.load() ) return nullptr;
        return std::make_unique<MockPlugin>( 2, 2, &live );
    } );
    check( proc->state() == twPluginSlotState::Missing,
           "losing the plugin again returns the slot to MISSING" );
    check( live.load() == 0, "...and releases the real instance" );
    {
        auto p0 = freezeTap( taps[0], 0, rate );
        check( p0 && nearly( p0->samples[9], sources[0]->value( 9 ), 1e-6 ),
               "...transparent once more" );
    }

    // A declared layout with no mapping AND no plugin: MISSING wins over
    // Unsupported, because "the plugin is not here" is the actionable report.
    {
        auto p = std::make_shared<twPluginSlotProcessor>(
            env, []() -> std::unique_ptr<twPlugin> { return nullptr; },
            twPluginIoLayout{ 3, 3 } );
        p->setBusCount( 2 );
        check( p->state() == twPluginSlotState::Missing,
               "an unmappable DECLARED layout with no plugin reports MISSING, not "
               "Unsupported" );
        check( p->mode() == twPluginSlotMode::Transparent,
               "...and stays transparent" );
    }

    return 0;
}

// ---------------------------------------------------------------------------
// M3: real audio through a slot. This is what the pre-M3 code could not do —
// twPluginInsert::freezePage wrote INTERLEAVED stereo into a page the engine
// reads as mono, and a 2-in plugin's second input was never wired at all.
static int testChainAudio()
{
    std::cout << "=== M3 audio through a two-bus slot ===" << std::endl;

    tw303aEnvironment env;
    env.setSRate( 48000 );
    const int      rate  = env.getSRate();
    const length_t pageN = (length_t)twOutputPage::FRAME_CAPACITY;

    Rig r = buildRig( env, 2, 2, 2 );

    auto p0 = freezeTap( r.taps[0], 0, rate );
    auto p1 = freezeTap( r.taps[1], 0, rate );
    if( !check( p0 && p1, "both taps produced a page" ) ) return 1;

    check( p0->validFrames == (uint32_t)pageN && p1->validFrames == (uint32_t)pageN,
           "each tap's page is a full mono page" );

    // MockPlugin: out[c] = in[c] * (c+1). Bus 0 is its own source at gain 1,
    // bus 1 is its own source at gain 2 — so a silent right input, a swapped
    // pair or an interleaved write all show up here.
    bool okL = true, okR = true, differ = false;
    for( length_t i = 0; i < 4096; ++i ) {
        const float wantL = r.sources[0]->value( i ) * 1.0f;
        const float wantR = r.sources[1]->value( i ) * 2.0f;
        okL = okL && nearly( p0->samples[i], wantL, 1e-6 );
        okR = okR && nearly( p1->samples[i], wantR, 1e-6 );
        if( !nearly( p0->samples[i], p1->samples[i], 1e-9 ) ) differ = true;
    }
    check( okL, "bus 0 carries its own upstream through the plugin" );
    check( okR, "bus 1 carries ITS OWN upstream through the plugin (not silence)" );
    check( differ, "the two buses are genuinely different audio" );

    // Chunking (CONTRACT invariant 5): the plugin never sees more than it was
    // activated for, and it really does see the declared block size.
    std::vector<twPlugin *> ps = r.proc->plugins();
    if( check( ps.size() == 1, "Direct mode has one instance" ) ) {
        MockPlugin *mp = static_cast<MockPlugin *>( ps[0] );
        check( mp->overruns_ == 0, "the plugin was never handed more than maxBlock" );
        check( mp->maxSeen_ == (std::uint32_t)twPluginSlotProcessor::kChunkFrames,
               "a 65536-frame page reaches the plugin as kChunkFrames blocks" );
        check( mp->prepares_ == 1, "prepare() ran exactly once for the slot" );
    }

    // A second page at the same position must be served from the caches, not
    // re-rendered.
    auto p0b = freezeTap( r.taps[0], 0, rate );
    check( p0b == p0, "re-requesting the same page hits the component cache" );

    // The next page in sequence must not reset the plugin (state continuity).
    auto n0 = freezeTap( r.taps[0], (offset_t)pageN, rate );
    if( check( n0 != nullptr, "the following page freezes" ) ) {
        check( nearly( n0->samples[3], r.sources[0]->value( (offset_t)pageN + 3 ), 1e-6 ),
               "the following page carries the audio for ITS position" );
    }

    // --- bypass must invalidate pages, or the toggle is inaudible ----------
    r.proc->setBypass( true );
    auto b0 = freezeTap( r.taps[0], 0, rate );
    check( b0 != p0, "toggling bypass stales the cached page" );
    check( b0 && nearly( b0->samples[9], r.sources[0]->value( 9 ), 1e-6 ),
           "a bypassed slot passes its input straight through" );
    r.proc->setBypass( false );
    auto u0 = freezeTap( r.taps[0], 0, rate );
    check( u0 && nearly( u0->samples[9], r.sources[0]->value( 9 ) * 1.0f, 1e-6 ),
           "un-bypassing brings the plugin back" );

    // --- a parameter edit must invalidate pages too ------------------------
    if( !ps.empty() ) {
        ps[0]->setParam( 0, 4.0 );
        r.proc->bumpParamEpoch();
        auto g0 = freezeTap( r.taps[0], 0, rate );
        check( g0 && nearly( g0->samples[9], r.sources[0]->value( 9 ) * 4.0f, 1e-5 ),
               "a parameter edit is audible on the next freeze (pages invalidated)" );
    }

    // --- a preview freeze must not touch the plugin ------------------------
    if( !ps.empty() ) {
        MockPlugin *mp = static_cast<MockPlugin *>( ps[0] );
        const int preparesBefore = mp->prepares_;
        auto pv = r.taps[0]->freezePage( 0, nullptr, 0, 1000, 1000, nullptr );
        check( mp->prepares_ == preparesBefore,
               "a preview freeze does NOT re-prepare the plugin (CONTRACT 6)" );
        check( pv && nearly( pv->samples[3], r.sources[0]->value( 3 ), 1e-6 ),
               "...and forwards the upstream envelope unprocessed" );
    }

    return 0;
}

// ---------------------------------------------------------------------------
// M3 hard invariant 1: two taps of one slot freezing CONCURRENTLY must not
// deadlock. Tap 0 renders holding the processor mutex and gathers bus 1 through
// tap 1's pullUpstreamPage(); if that took tap 1's own component mutex — or if a
// tap held its mutex across pageFor() — this hangs. A hang is reported as a
// failure and the process is aborted, because a deadlocked thread cannot be
// joined.
static int testConcurrentTapFreeze()
{
    std::cout << "=== M3 concurrent two-tap freeze (deadlock gate) ===" << std::endl;

    tw303aEnvironment env;
    env.setSRate( 48000 );
    const int      rate  = env.getSRate();
    const length_t pageN = (length_t)twOutputPage::FRAME_CAPACITY;

    Rig r = buildRig( env, 2, 2, 2 );

    std::atomic<int>  mismatches{ 0 };
    std::atomic<bool> done{ false };

    auto body = [&]() {
        const int kIters = 120;
        for( int it = 0; it < kIters; ++it ) {
            // Force BOTH taps to actually render this round.
            r.proc->bumpParamEpoch();
            const offset_t pos = (offset_t)( ( it % 3 ) * pageN );

            std::atomic<int> ready{ 0 };
            std::shared_ptr<twOutputPage> pages[2];

            auto worker = [&]( int bus ) {
                ready.fetch_add( 1 );
                while( ready.load() < 2 ) { /* line the two threads up */ }
                pages[bus] = freezeTap( r.taps[bus], pos, rate );
            };

            std::thread t0( worker, 0 );
            std::thread t1( worker, 1 );
            t0.join();
            t1.join();

            for( int bus = 0; bus < 2; ++bus ) {
                if( !pages[bus] ) { mismatches.fetch_add( 1 ); continue; }
                const float want = r.sources[bus]->value( pos + 11 ) * (float)( bus + 1 );
                if( !nearly( pages[bus]->samples[11], want, 1e-5 ) )
                    mismatches.fetch_add( 1 );
            }
        }
        done.store( true );
    };

    std::future<void> fut = std::async( std::launch::async, body );
    if( fut.wait_for( std::chrono::seconds( 60 ) ) != std::future_status::ready ) {
        check( false, "concurrent two-tap freezes complete without deadlocking" );
        std::cerr << "=== DEADLOCK: aborting (a hung thread cannot be joined) ==="
                  << std::endl;
        std::cerr.flush();
        std::cout.flush();
        std::_Exit( 1 );
    }
    fut.get();

    check( done.load(), "concurrent two-tap freezes complete without deadlocking" );
    check( mismatches.load() == 0,
           "every concurrently frozen page carries the right bus at the right position" );

    return 0;
}

#ifdef TW_TESTCLAP_PATH

// The real CLAP load path, against the in-repo fixture module (twtestclap.c).
// This is the M1 gate: LoadLibrary/dlopen -> clap_entry -> factory ->
// activate/start_processing -> process -> parameter events -> state blob.
static int testClapBackend()
{
    std::cout << "=== CLAP backend (" << TW_TESTCLAP_PATH << ") ===" << std::endl;

    tw303aEnvironment env;
    auto             &registry = pluginRegistry();

    // The scanner is M2, so the descriptor is built by hand here — which is
    // exactly the shape M2 will produce.
    twPluginDescriptor desc;
    desc.format = "clap";
    desc.uid    = "tw.test.clap.gain";
    desc.path   = TW_TESTCLAP_PATH;
    desc.name   = "Smaragd Test Gain";

    std::unique_ptr<twPlugin> plugin = registry.instantiate( desc );
    if( !check( plugin != nullptr, "registry instantiates a format=\"clap\" descriptor" ) )
        return 1;

    check( plugin->ioLayout().audioInputs == 2 && plugin->ioLayout().audioOutputs == 2,
           "clap.audio-ports reports the main port as 2-in / 2-out" );
    // THREE since proposal 36 P2: Gain, Report Block Size and the Clip
    // Threshold the fader-move ORDER case needs (id 2 — id 1 was already the
    // block-size reporter; see the fixture's header comment).
    check( plugin->paramCount() == 3, "clap.params reports 3 parameters" );
    check( plugin->paramInfo( 0 ).name == "Gain", "parameter 0 is named Gain" );
    check( plugin->paramInfo( 1 ).isStepped, "parameter 1 is stepped" );
    check( plugin->paramInfo( 2 ).name == "Clip Threshold", "parameter 2 is the clipper" );
    check( nearly( plugin->paramInfo( 2 ).defaultValue, 0.0 ),
           "the clipper defaults to OFF, so no existing render changes" );
    check( nearly( plugin->getParam( 0 ), 1.0 ), "Gain reads its default of 1.0" );
    check( plugin->reportedLatency() == 0, "clap.latency reports 0" );

    // --- paramValueText(): the plugin's own value-to-text formatting -------
    // The fixture's tc_params_value_to_text formats value*100 as an integer, so
    // this proves the ABI virtual reaches clap.params.value_to_text, the buffer
    // round-trips, and the empty/non-empty contract holds — the display path.
    check( plugin->paramValueText( 0, 1.0 ) == "100",
           "paramValueText formats via the plugin's value_to_text" );
    check( plugin->paramValueText( 0, 2.0 ) == "200",
           "paramValueText tracks the passed value" );

    // --- process(): the default unity gain -------------------------------
    const std::uint32_t   n = 512;
    std::vector<float>    inL( n ), inR( n ), outL( n ), outR( n );
    for( std::uint32_t i = 0; i < n; ++i ) {
        inL[i] = 0.25f + 0.001f * (float)i;
        inR[i] = -0.5f;
    }
    const float *ins[2]  = { inL.data(), inR.data() };
    float       *outs[2] = { outL.data(), outR.data() };

    plugin->prepare( 48000, twPluginInsert::kChunkFrames );
    plugin->process( ins, outs, n );
    bool unity = true;
    for( std::uint32_t i = 0; i < n; ++i )
        unity = unity && nearly( outL[i], inL[i] ) && nearly( outR[i], inR[i] );
    check( unity, "process() at unity gain reproduces the input" );

    // --- setParam(): the event ring must reach the plugin ------------------
    // setParam() never calls the plugin; the value has to travel as a
    // CLAP_EVENT_PARAM_VALUE inside the next process() call.
    plugin->setParam( 0, 2.0 );
    check( nearly( plugin->getParam( 0 ), 2.0 ), "getParam reflects the edit immediately" );
    plugin->process( ins, outs, n );
    bool doubled = true;
    for( std::uint32_t i = 0; i < n; ++i )
        doubled = doubled && nearly( outL[i], inL[i] * 2.0f, 1e-5 );
    check( doubled, "a queued CLAP_EVENT_PARAM_VALUE reaches the plugin through process()" );

    // --- clap.state through our versioned 8-byte frame --------------------
    const std::vector<std::uint8_t> saved = plugin->saveState();
    check( saved.size() == 8 + 16, "state blob is our 8-byte header plus the plugin payload" );
    check( saved[0] == 'T' && saved[1] == 'W' && saved[2] == 'C' && saved[3] == 'P',
           "state blob carries the TWCP magic" );

    plugin->setParam( 0, 0.25 );
    plugin->process( ins, outs, n );   // let the edit land in the plugin
    check( plugin->loadState( saved ), "loadState accepts our own blob" );
    check( nearly( plugin->getParam( 0 ), 2.0 ),
           "loadState restores the saved value and refreshes the host mirror" );

    // Version tolerance (CONTRACT invariant 3): a blob from the future is
    // refused rather than misread, and a foreign/truncated blob cannot crash us.
    std::vector<std::uint8_t> future = saved;
    future[4] = 99;
    check( !plugin->loadState( future ), "a newer state version is refused" );
    check( !plugin->loadState( std::vector<std::uint8_t>{ 1, 2, 3 } ),
           "a truncated state blob is refused" );
    std::vector<std::uint8_t> foreign = saved;
    foreign[0] = 'X';
    check( !plugin->loadState( foreign ), "a foreign state magic is refused" );

    // --- host-side chunking through twPluginInsert -------------------------
    // A page is twOutputPage::FRAME_CAPACITY frames; the plugin was activated
    // for kChunkFrames. The fixture returns CLAP_PROCESS_ERROR if it is ever
    // handed more than that, and in "report" mode writes the frame count it saw
    // into every output sample — so the assertion below reads the chunk size the
    // plugin actually observed, not the one we hoped for.
    std::unique_ptr<twPlugin> chunkPlugin = registry.instantiate( desc );
    if( check( chunkPlugin != nullptr, "second CLAP instance shares the loaded module" ) ) {
        twPlugin *raw    = chunkPlugin.get();
        auto      insert = std::make_unique<twPluginInsert>( env, std::move( chunkPlugin ) );
        raw->setParam( 1, 1.0 );   // report-block-size mode

        const length_t pageFrames = (length_t)twOutputPage::FRAME_CAPACITY;
        std::vector<float> seed( (std::size_t)pageFrames, -1.0f );
        // CreateFromBuffer COPIES into a temporary page, so the result has to be
        // read back through the IOVector, not out of `seed`.
        IOVector dv = IOVector::CreateFromBuffer( seed.data(), pageFrames );
        insert->calcOutputTo( dv, 0 );
        const float *got = dv.rawPointer();

        float lo = got[0], hi = got[0];
        for( length_t i = 1; i < pageFrames; ++i ) {
            lo = std::fmin( lo, got[i] );
            hi = std::fmax( hi, got[i] );
        }
        check( nearly( lo, (double)twPluginInsert::kChunkFrames ) &&
                   nearly( hi, (double)twPluginInsert::kChunkFrames ),
               "a 65536-frame page reaches the plugin as kChunkFrames-sized blocks" );
    }

    // --- M3: the second fixture plugin, whose DEFAULT behaviour is per-channel
    // asymmetric. plugin_stereo_chain.qxa depends on exactly this, since a qxa
    // script cannot set a parameter before M5.
    twPluginDescriptor skew = desc;
    skew.uid  = "tw.test.clap.stereoskew";
    skew.name = "Smaragd Test Stereo Skew";
    std::unique_ptr<twPlugin> sp = registry.instantiate( skew );
    if( check( sp != nullptr, "the module also exports tw.test.clap.stereoskew" ) ) {
        check( sp->ioLayout().audioInputs == 2 && sp->ioLayout().audioOutputs == 2,
               "the skew fixture is 2-in / 2-out too" );
        sp->prepare( 48000, twPluginInsert::kChunkFrames );
        sp->process( ins, outs, n );
        bool skewed = true;
        for( std::uint32_t i = 0; i < n; ++i )
            skewed = skewed &&
                     nearly( outL[i], inL[i] * 0.5f + inR[i], 1e-6 ) &&
                     nearly( outR[i], inR[i] * 0.5f, 1e-6 );
        check( skewed,
               "the skew fixture cross-mixes into channel 0 and halves channel 1, "
               "with no parameter set" );

        // The property plugin_stereo_chain.qxa leans on: with two IDENTICAL mono
        // buses (what a track produces) channel 0 comes out at 1.5x — and at
        // 0.5x if input 1 were silent, which is the bug being guarded.
        const float *same[2] = { inL.data(), inL.data() };
        sp->process( same, outs, n );
        bool oneAndAHalf = true;
        for( std::uint32_t i = 0; i < n; ++i )
            oneAndAHalf = oneAndAHalf && nearly( outL[i], inL[i] * 1.5f, 1e-6 );
        check( oneAndAHalf,
               "two identical inputs give channel 0 at 1.5x (0.5x would mean a "
               "silent input 1)" );
    }

    return 0;
}

#endif  // TW_TESTCLAP_PATH

#ifdef TW_TESTVST3_PATH

// The real VST3 load path, against the in-repo fixture module (twtestvst3.cpp).
// This is the M6 gate: LoadLibrary/dlopen -> InitDll -> GetPluginFactory ->
// IComponent/IAudioProcessor/IEditController -> setActive/setProcessing ->
// process -> parameter changes -> the two-chunk state blob.
static int testVst3Backend()
{
    std::cout << "=== VST3 backend (" << TW_TESTVST3_PATH << ") ===" << std::endl;

    tw303aEnvironment env;
    auto             &registry = pluginRegistry();

    // An empty uid means "the first audio-effect class", which is what a
    // one-class module makes unambiguous — and it exercises the path a probe
    // takes before any uid is known.
    twPluginDescriptor desc;
    desc.format = "vst3";
    desc.uid    = "";
    desc.path   = TW_TESTVST3_PATH;
    desc.name   = "TW Test VST3 Gain";

    std::unique_ptr<twPlugin> plugin = registry.instantiate( desc );
    if( !check( plugin != nullptr, "registry instantiates a format=\"vst3\" descriptor" ) )
        return 1;

    check( plugin->ioLayout().audioInputs == 2 && plugin->ioLayout().audioOutputs == 2,
           "the main audio buses report 2-in / 2-out" );
    check( plugin->paramCount() == 1, "IEditController reports 1 parameter" );
    check( plugin->paramInfo( 0 ).name == "Gain", "parameter 0 is named Gain" );
    // VST3 parameters are normalized at the interface and this backend keeps
    // them that way — see the PARAMETER DOMAIN note in twvst3plugin.cc.
    check( nearly( plugin->paramInfo( 0 ).minValue, 0.0 ) &&
               nearly( plugin->paramInfo( 0 ).maxValue, 1.0 ),
           "parameters are exposed in the normalized [0,1] domain" );
    check( nearly( plugin->getParam( 0 ), 1.0 ), "Gain reads its default of 1.0" );
    check( plugin->reportedLatency() == 0, "getLatencySamples reports 0" );

    // --- process(): the default unity gain ---------------------------------
    const std::uint32_t n = 512;
    std::vector<float>  inL( n ), inR( n ), outL( n ), outR( n );
    for( std::uint32_t i = 0; i < n; ++i ) {
        inL[i] = 0.25f + 0.001f * (float)i;
        inR[i] = -0.5f;
    }
    const float *ins[2]  = { inL.data(), inR.data() };
    float       *outs[2] = { outL.data(), outR.data() };

    plugin->prepare( 48000, twPluginInsert::kChunkFrames );
    plugin->process( ins, outs, n );
    bool unity = true;
    for( std::uint32_t i = 0; i < n; ++i )
        unity = unity && nearly( outL[i], inL[i] ) && nearly( outR[i], inR[i] );
    check( unity, "process() at unity gain reproduces the input" );

    // --- setParam(): the ONLY route to the DSP is inputParameterChanges -----
    // The fixture deliberately ignores setParamNormalized, so this assertion
    // fails for a host that writes the controller and stops there — the single
    // most common VST3 host bug, and the reason the fixture is built that way.
    plugin->setParam( 0, 0.5 );
    check( nearly( plugin->getParam( 0 ), 0.5 ), "getParam reflects the edit immediately" );
    plugin->process( ins, outs, n );
    bool halved = true;
    for( std::uint32_t i = 0; i < n; ++i )
        halved = halved && nearly( outL[i], inL[i] * 0.5f, 1e-5 ) &&
                 nearly( outR[i], inR[i] * 0.5f, 1e-5 );
    check( halved,
           "a queued parameter point reaches the processor through "
           "ProcessData::inputParameterChanges" );

    // --- state through our versioned frame ---------------------------------
    // 8-byte header + two length-prefixed chunks (component, controller). The
    // fixture stores 4 bytes of magic and an 8-byte double, and has no separate
    // controller state.
    const std::vector<std::uint8_t> saved = plugin->saveState();
    check( saved[0] == 'T' && saved[1] == 'W' && saved[2] == 'V' && saved[3] == '3',
           "state blob carries the TWV3 magic" );
    // Assert the FRAMING, not a magic total: 8-byte header, then two
    // length-prefixed chunks that must account for every remaining byte.
    if( check( saved.size() >= 8 + 4 + 4, "state blob has room for both chunk headers" ) ) {
        auto u32At = []( const std::vector<std::uint8_t> &b, std::size_t at ) {
            return (std::uint32_t)b[at] | ( (std::uint32_t)b[at + 1] << 8 ) |
                   ( (std::uint32_t)b[at + 2] << 16 ) | ( (std::uint32_t)b[at + 3] << 24 );
        };
        const std::uint32_t compLen = u32At( saved, 8 );
        const std::size_t   ctrlAt  = 8 + 4 + compLen;
        if( check( ctrlAt + 4 <= saved.size(), "the component chunk fits inside the blob" ) ) {
            const std::uint32_t ctrlLen = u32At( saved, ctrlAt );
            check( ctrlAt + 4 + ctrlLen == saved.size(),
                   "the two chunks account for exactly the whole blob" );
            // 4 bytes of magic + an 8-byte double, from twtestvst3.cpp.
            check( compLen == 12, "the component chunk is the fixture's 12-byte payload" );
            // A SINGLE-COMPONENT plugin has no controller state of its own:
            // IComponent::getState and IEditController::getState are the same
            // virtual, so storing it twice would be pure duplication.
            check( ctrlLen == 0, "a single-component plugin stores no controller chunk" );
        }
    }

    plugin->setParam( 0, 0.25 );
    plugin->process( ins, outs, n );   // let the edit land in the plugin
    check( plugin->loadState( saved ), "loadState accepts our own blob" );
    check( nearly( plugin->getParam( 0 ), 0.5 ),
           "loadState restores the saved value and refreshes the host mirror" );

    // Version tolerance (CONTRACT invariant 3): a blob from the future is
    // refused rather than misread, and a foreign/truncated blob cannot crash us.
    std::vector<std::uint8_t> future = saved;
    future[4] = 99;
    check( !plugin->loadState( future ), "a newer state version is refused" );
    check( !plugin->loadState( std::vector<std::uint8_t>{ 1, 2, 3 } ),
           "a truncated state blob is refused" );
    std::vector<std::uint8_t> foreign = saved;
    foreign[0] = 'X';
    check( !plugin->loadState( foreign ), "a foreign state magic is refused" );
    // A CLAP blob must never be readable as a VST3 one, and vice versa: the
    // magics differ precisely so a mis-routed blob is refused, not misread.
    std::vector<std::uint8_t> clapish = saved;
    clapish[2] = 'C';
    clapish[3] = 'P';
    check( !plugin->loadState( clapish ), "a CLAP-framed blob is refused by the VST3 backend" );
    // Truncated CHUNK header (well-formed frame, lying length).
    std::vector<std::uint8_t> shortChunk( saved.begin(), saved.begin() + 8 + 4 + 2 );
    check( !plugin->loadState( shortChunk ), "a blob whose chunk runs past the end is refused" );

    // --- a second instance shares the loaded module -------------------------
    std::unique_ptr<twPlugin> second = registry.instantiate( desc );
    if( check( second != nullptr, "second VST3 instance shares the interned module" ) ) {
        second->prepare( 48000, twPluginInsert::kChunkFrames );
        second->process( ins, outs, n );
        bool independent = true;
        for( std::uint32_t i = 0; i < n; ++i )
            independent = independent && nearly( outL[i], inL[i] );
        check( independent, "the second instance has its own parameter state (unity)" );
    }

    // --- resolving by explicit uid ------------------------------------------
    // What a saved project does: the hex class id round-trips through the
    // descriptor and finds the same class.
    // TWO audio-effect classes since proposal 36 P2 (the gain effect and the
    // sine instrument); the instrument's separate CONTROLLER class carries
    // kVstComponentControllerClass and must NOT be enumerated as a plugin.
    const std::vector<twPluginDescriptor> found = vst3ModuleDescriptors( TW_TESTVST3_PATH );
    check( found.size() == 2, "the module reports exactly two audio-effect classes" );
    const twPluginDescriptor *gainDesc = nullptr;
    const twPluginDescriptor *sineDesc = nullptr;
    for( const twPluginDescriptor &d : found ) {
        if( d.name == "TW Test VST3 Gain" ) gainDesc = &d;
        if( d.name == "TW Test VST3 Sine" ) sineDesc = &d;
    }
    if( check( gainDesc != nullptr, "the gain class is in the descriptor list" ) ) {
        check( gainDesc->format == "vst3", "descriptor format is vst3" );
        check( gainDesc->uid.size() == 32, "uid is a 32-hex-digit class id" );
        check( gainDesc->vendor == "Smaragd", "descriptor carries the vendor" );
        check( !gainDesc->isInstrument, "an Fx subcategory is not an instrument" );
        check( gainDesc->io.audioInputs == 2 && gainDesc->io.audioOutputs == 2,
               "descriptor I/O comes from a live instance" );
        check( !gainDesc->acceptsNotes, "the effect declares no event bus" );
        check( gainDesc->nOutBuses == 1, "the effect has one output bus" );

        twPluginDescriptor byUid = *gainDesc;
        std::unique_ptr<twPlugin> resolved = registry.instantiate( byUid );
        check( resolved != nullptr, "a descriptor resolved by uid instantiates" );
    }
    if( check( sineDesc != nullptr, "the SPLIT instrument class is in the list" ) ) {
        check( sineDesc->isInstrument,
               "an Instrument|Synth subcategory IS an instrument" );
        check( sineDesc->io.audioInputs == 0 && sineDesc->io.audioOutputs == 2,
               "the instrument declares 0 in / stereo out" );
        check( sineDesc->acceptsNotes && sineDesc->eventPortsIn == 1,
               "the scanner records its kEvent input bus (scanner version 2)" );
        check( !sineDesc->emitsNotes, "...and that it emits none" );
    }

    // A path that is not a plugin must fail cleanly, not crash: this is the
    // property the out-of-process probe depends on for corrupt files.
    check( vst3ModuleDescriptors( "definitely-not-a-module.vst3" ).empty(),
           "a missing module yields no descriptors" );

    return 0;
}

#endif  // TW_TESTVST3_PATH

// ===========================================================================
// Proposal 36 P2 — the EVENT half of the ABI, driven DIRECTLY on twPlugin.
//
// Nothing below goes through twPluginSlotProcessor or twPluginInsert. That is
// deliberate and it is what the phase brief asks for: P2 changes the ABI and
// the backends and NOTHING about the hosting components (proposal 35-B4
// rewrites those, and P3b owns the generator modes). So the harness is a plain
// block pump — 4096-frame calls with an event list — which is exactly the shape
// the processor will use once it exists.
// ===========================================================================

namespace ev36 {

constexpr std::uint32_t kBlock = 4096;
constexpr std::uint32_t kRate  = 48000;

// MIDI velocity 100, normalised — the ABI carries velocity in [0,1]
// (twpluginevents.h), and "velocity 100" in the acceptance criteria is the MIDI
// spelling of it. It is also exactly the 303's accent threshold.
constexpr double kVel100 = 100.0 / 127.0;

// C4. 440 * 2^((60-69)/12) = 261.6256 Hz.
constexpr int    kKeyC4 = 60;
constexpr double kHzC4  = 261.6255653005986;

twEvent noteOn( std::int64_t t, int key, double vel, std::int32_t id )
{
    twEvent e;
    e.time    = t;
    e.kind    = twEventKind::NoteOn;
    e.channel = 0;
    e.key     = (std::int16_t)key;
    e.noteId  = id;
    e.value   = vel;
    return e;
}

twEvent noteOff( std::int64_t t, int key, std::int32_t id )
{
    twEvent e;
    e.time    = t;
    e.kind    = twEventKind::NoteOff;
    e.channel = 0;
    e.key     = (std::int16_t)key;
    e.noteId  = id;   // the host issues the id and the OFF carries the same one
    e.value   = 0.0;
    return e;
}

twEvent paramValue( std::int64_t t, std::uint32_t id, double v )
{
    twEvent e;
    e.time    = t;
    e.kind    = twEventKind::ParamValue;
    e.paramId = id;
    e.value   = v;
    return e;
}

// One rendered take: channel 0 of the main bus, plus whatever the plugin pushed
// back through twEventOut.
struct Take {
    std::vector<float>   mono;
    std::vector<twEvent> out;
    std::uint32_t        dropped = 0;
};

// Render `totalFrames` in kBlock-sized calls, delivering `events` (absolute
// frame times) rebased to each block. This IS the contract under test: the list
// a plugin sees is chunk-relative and sorted.
Take render( twPlugin &p, const std::vector<twEvent> &events,
             std::uint32_t totalFrames, std::uint32_t outChannels = 2 )
{
    Take take;
    take.mono.resize( totalFrames, 0.0f );

    std::vector<std::vector<float>> outBufs( outChannels,
                                             std::vector<float>( kBlock, 0.0f ) );
    std::vector<float *> outPtrs( outChannels );
    for( std::uint32_t c = 0; c < outChannels; ++c ) outPtrs[c] = outBufs[c].data();
    float *const *outBuses[1] = { outPtrs.data() };

    // Host-owned event storage, sized once — never inside the loop, which is
    // the ABI's own rule (no allocation on the render path).
    std::vector<twEvent>      inStore( twEventLimits::kMaxEventsPerBlock );
    std::vector<twEvent>      outStore( twEventLimits::kMaxEventsPerBlock );
    std::vector<std::uint8_t> outArena( 4096 );
    twEventOut               sink;
    sink.setStorage( outStore.data(), (std::uint32_t)outStore.size(),
                     outArena.data(), (std::uint32_t)outArena.size() );

    std::size_t next = 0;
    for( std::uint32_t start = 0; start < totalFrames; start += kBlock ) {
        const std::uint32_t n = std::min( kBlock, totalFrames - start );

        std::uint32_t count = 0;
        while( next < events.size() &&
               events[next].time < (std::int64_t)( start + n ) ) {
            twEvent e = events[next];
            e.time -= (std::int64_t)start;   // CHUNK-RELATIVE, 0..n-1
            inStore[count++] = e;
            ++next;
        }
        twEventList list;
        list.events = inStore.data();
        list.count  = count;

        sink.clear();
        for( std::uint32_t c = 0; c < outChannels; ++c )
            std::fill( outBufs[c].begin(), outBufs[c].end(), 0.0f );

        twProcessContext ctx;
        ctx.position   = (std::int64_t)start;
        ctx.playing    = true;
        ctx.tempoBpm   = 120.0;
        ctx.validFlags = twCtxPosition | twCtxTempo | twCtxTimeSig;

        p.process( nullptr, outBuses, n, list, sink, ctx );

        std::copy( outBufs[0].begin(), outBufs[0].begin() + n,
                   take.mono.begin() + start );
        for( std::uint32_t i = 0; i < sink.count(); ++i ) {
            twEvent e = sink.at( i );
            e.time += (std::int64_t)start;   // back to absolute, for counting
            take.out.push_back( e );
        }
        take.dropped += sink.dropped();
    }
    return take;
}

double peakIn( const std::vector<float> &v, std::size_t from, std::size_t to )
{
    double p = 0.0;
    for( std::size_t i = from; i < to && i < v.size(); ++i )
        p = std::max( p, (double)std::fabs( v[i] ) );
    return p;
}

double rmsIn( const std::vector<float> &v, std::size_t from, std::size_t to )
{
    double sum = 0.0;
    std::size_t n = 0;
    for( std::size_t i = from; i < to && i < v.size(); ++i ) {
        sum += (double)v[i] * (double)v[i];
        ++n;
    }
    return n ? std::sqrt( sum / (double)n ) : 0.0;
}

// Fundamental by autocorrelation over `n` frames, with PARABOLIC INTERPOLATION
// around the peak lag.
//
// The interpolation is not polish: at 48 kHz a 261.6 Hz period is 183.5
// samples, so integer lags alone resolve only to about +/- 0.8 Hz — which would
// sit right on the +/- 1 Hz acceptance band and make the test's verdict depend
// on which side of a sample the period happened to fall. Sub-sample lag makes
// the band mean what it says.
double fundamentalHz( const std::vector<float> &v, std::size_t from, std::size_t n )
{
    if( from + n > v.size() ) return 0.0;
    const std::size_t minLag = kRate / 2000;   // 2 kHz ceiling
    const std::size_t maxLag = kRate / 50;     // 50 Hz floor
    if( n <= maxLag + 2 ) return 0.0;

    auto corr = [&]( std::size_t lag ) {
        double s = 0.0;
        for( std::size_t i = 0; i + lag < n; ++i )
            s += (double)v[from + i] * (double)v[from + i + lag];
        return s;
    };

    std::size_t best = 0;
    double      bestV = -1e30;
    for( std::size_t lag = minLag; lag <= maxLag; ++lag ) {
        const double c = corr( lag );
        if( c > bestV ) { bestV = c; best = lag; }
    }
    if( best <= minLag || best >= maxLag ) return 0.0;

    const double ym = corr( best - 1 ), y0 = bestV, yp = corr( best + 1 );
    const double denom = ( ym - 2.0 * y0 + yp );
    double delta = 0.0;
    if( std::fabs( denom ) > 1e-30 )
        delta = 0.5 * ( ym - yp ) / denom;
    if( delta < -1.0 || delta > 1.0 ) delta = 0.0;
    return (double)kRate / ( (double)best + delta );
}

}  // namespace ev36

// --- AC1 / AC5: one instrument, one protocol -------------------------------
//
// `sineLike` says whether the fixture is one of the ENVELOPE-LESS sine
// instruments, whose steady state has no memory beyond the held-note set. Those
// get the exact assertions: an RMS closed form (+/- 2 %) and EXACT silence after
// the note-off. The 303 has an envelope and filter memory by design, so per
// design D7 it gates PRESENCE and warmth instead — its frequency must be right,
// its level must be real, and its tail must decay.
static void checkInstrument( twPlugin &plugin, const char *label, bool sineLike )
{
    using namespace ev36;

    plugin.prepare( kRate, kBlock );
    plugin.reset();

    check( plugin.capabilities().acceptsNotes,
           std::string( std::string( label ) + ": declares a note input" ).c_str() );

    // AC1. NoteOn(60, vel 100) at offset 1000; NoteOff at 30000.
    const std::uint32_t kOn    = 1000;
    const std::uint32_t kOff   = 30000;
    const std::uint32_t kTotal = kBlock * 12;   // 49152 frames
    std::vector<twEvent> evs = { noteOn( kOn, kKeyC4, kVel100, 7 ),
                                 noteOff( kOff, kKeyC4, 7 ) };

    const Take t = render( plugin, evs, kTotal );

    check( peakIn( t.mono, 0, kOn ) < 1e-6,
           std::string( std::string( label ) + ": EXACT silence before the note-on"
                        ).c_str() );

    // The 303's filter and envelope need a moment to settle into the periodic
    // steady state; the sines are periodic from the first sample.
    const std::size_t anaFrom = kOn + ( sineLike ? 0u : kBlock );
    const double hz = fundamentalHz( t.mono, anaFrom, kBlock );
    check( std::fabs( hz - kHzC4 ) <= 1.0,
           std::string( std::string( label ) + ": fundamental is 261.6 +/- 1 Hz (got "
                        + std::to_string( hz ) + ")" ).c_str() );

    if( sineLike ) {
        // Closed form: one voice, amp = velocity, gain 1 => RMS = vel / sqrt(2).
        const double want = kVel100 / std::sqrt( 2.0 );
        const double got  = rmsIn( t.mono, kOn + 512, kOn + 512 + kBlock * 4 );
        check( std::fabs( got - want ) <= want * 0.02,
               std::string( std::string( label ) + ": RMS is vel/sqrt(2) +/- 2 % (want "
                            + std::to_string( want ) + ", got " + std::to_string( got )
                            + ")" ).c_str() );
        check( peakIn( t.mono, kOff + 1, kTotal ) < 1e-6,
               std::string( std::string( label ) + ": EXACT silence after the note-off"
                            ).c_str() );
    } else {
        // D7: the 303 has an envelope and filter memory, so it gates PRESENCE
        // and warmth rather than an RMS closed form.
        const double held = rmsIn( t.mono, kOn + kBlock, kOn + kBlock * 3 );
        check( held > 0.05,
               std::string( std::string( label ) + ": the held note is audible (RMS "
                            + std::to_string( held ) + " > 0.05)" ).c_str() );
        // Its VCA is a gate with a 6 ms release (the Decay knob sweeps the
        // FILTER, as on the instrument itself), so 512 frames after the note-off
        // the output is exactly zero — the same sharp assertion the sines get,
        // just displaced by the release.
        check( peakIn( t.mono, kOff + 512, kTotal ) < 1e-6,
               std::string( std::string( label ) +
                            ": silence once the 6 ms VCA release has run out"
                            ).c_str() );
    }

    // AC5. reset(), NoteOn at offset 0, 8192 frames, twice => byte-identical.
    // Deterministic reset is a FIXTURE REQUIREMENT, not an observation: every
    // one of these instruments sets phase, voice table and filter state to fixed
    // values in reset(), which is what makes the later render-vs-render byte
    // gates of P3c possible at all.
    const std::vector<twEvent> one = { noteOn( 0, kKeyC4, kVel100, 1 ) };
    plugin.reset();
    const Take a = render( plugin, one, kBlock * 2 );
    plugin.reset();
    const Take b = render( plugin, one, kBlock * 2 );
    check( a.mono.size() == b.mono.size() &&
               std::memcmp( a.mono.data(), b.mono.data(),
                            a.mono.size() * sizeof( float ) ) == 0,
           std::string( std::string( label ) +
                        ": reset + note-on + 8192 frames is byte-identical twice"
                        ).c_str() );
}

static int testEventAbi()
{
    using namespace ev36;

    std::cout << "=== proposal 36 P2: events at the twPlugin level ===" << std::endl;

    auto &registry = pluginRegistry();

    // --- AC1/AC5 (a): the in-repo native 303 -------------------------------
    {
        twPluginDescriptor d;
        d.format = "tw";
        d.uid    = "tw.native.303";
        std::unique_ptr<twPlugin> p = registry.instantiate( d );
        if( check( p != nullptr, "the registry instantiates tw.native.303" ) ) {
            check( p->capabilities().isInstrument, "the 303 declares itself an instrument" );
            check( p->ioLayout().audioInputs == 0 && p->ioLayout().audioOutputs == 1,
                   "the 303 is a 0-in / 1-out generator" );
            check( p->audioOutBusCount() == 1, "...with a single output bus" );
            check( p->tailFrames() > 0, "...and reports a tail (its decay)" );
            checkInstrument( *p, "303", /*sineLike=*/false );
        }

        // It is also in the REGISTRY LIST, like twPassThrough — that is how the
        // browser will find it (AC7).
        bool listed = false;
        for( const twPluginDescriptor &e : registry.plugins() )
            if( e.uid == "tw.native.303" ) {
                listed = true;
                check( e.isInstrument, "the registry lists the 303 with isInstrument" );
                check( e.acceptsNotes && e.eventPortsIn == 1,
                       "...and with its event input port" );
                check( e.format == "tw" && e.path.empty(),
                       "...as a linked-in tw plugin with no module path" );
            }
        check( listed, "tw.native.303 is a built-in of the registry" );
    }

#ifdef TW_TESTCLAP_PATH
    // --- AC1/AC5 (b): the CLAP sine instrument -----------------------------
    {
        twPluginDescriptor d;
        d.format = "clap";
        d.uid    = "tw.test.clap.sine";
        d.path   = TW_TESTCLAP_PATH;
        std::unique_ptr<twPlugin> p = registry.instantiate( d );
        if( check( p != nullptr, "the CLAP sine instrument instantiates" ) ) {
            const twPluginCapabilities c = p->capabilities();
            check( c.isInstrument, "clap: the INSTRUMENT feature is read" );
            check( c.acceptsNotes && c.notePortsIn == 1, "clap: one note input port" );
            check( !c.emitsNotes, "clap: the sine emits no notes" );
            // The fixture offers CLAP|MIDI and prefers CLAP; a host that picked
            // MIDI would lose the note ids, so the negotiation is asserted.
            check( c.supportsNoteIds && !c.wantsMidi1Raw,
                   "clap: the CLAP dialect was negotiated (note ids available)" );
            check( c.supportsNoteExpression, "clap: ...so note expressions are too" );
            check( p->audioOutBusCount() == 2,
                   "clap: the main stereo out AND the aux out are discovered" );
            check( p->audioOutBus( 0 ).channels == 2 && p->audioOutBus( 0 ).isMain,
                   "clap: bus 0 is the stereo MAIN out" );
            check( p->audioOutBus( 1 ).channels == 1 && !p->audioOutBus( 1 ).isMain,
                   "clap: bus 1 is the mono aux out" );
            checkInstrument( *p, "clap sine", /*sineLike=*/true );
        }
    }

    // --- AC4: the arpeggiator's event OUT ----------------------------------
    {
        twPluginDescriptor d;
        d.format = "clap";
        d.uid    = "tw.test.clap.arp";
        d.path   = TW_TESTCLAP_PATH;
        std::unique_ptr<twPlugin> p = registry.instantiate( d );
        if( check( p != nullptr, "the CLAP arpeggiator instantiates" ) ) {
            const twPluginCapabilities c = p->capabilities();
            check( c.acceptsNotes && c.emitsNotes,
                   "arp: note ports in AND out are discovered" );
            check( c.notePortsIn == 1 && c.notePortsOut == 1, "arp: one of each" );

            p->prepare( kRate, kBlock );
            p->reset();

            // ONE held key from frame 0, never released, over 65536 frames.
            const std::uint32_t kTotal = 65536;
            const std::vector<twEvent> held = { noteOn( 0, kKeyC4, 0.8, 42 ) };
            // The arp has no audio ports at all, so there is no output bus to
            // write; only the event lane is under test.
            const Take t = render( *p, held, kTotal, /*outChannels=*/1 );

            // CLOSED FORM. The grid is 4096 frames with a 2048-frame gate, so a
            // key held from frame 0 produces a note-on at 0, 4096, ... and a
            // matching note-off 2048 later; 2048 < 4096, so they never overlap.
            const std::uint32_t kGrid = 4096, kGate = 2048;
            const std::uint32_t wantOn  = ( kTotal + kGrid - 1 ) / kGrid;
            std::uint32_t       wantOff = 0;
            for( std::uint32_t on = 0; on < kTotal; on += kGrid )
                if( on + kGate < kTotal ) ++wantOff;

            std::uint32_t gotOn = 0, gotOff = 0;
            for( const twEvent &e : t.out ) {
                if( e.kind == twEventKind::NoteOn )  ++gotOn;
                if( e.kind == twEventKind::NoteOff ) ++gotOff;
            }
            check( t.dropped == 0, "arp: the host sink never overflowed" );
            check( gotOn == wantOn,
                   ( "arp: note-ons over 65536 frames == ceil(N/grid) == "
                     + std::to_string( wantOn ) + " (got " + std::to_string( gotOn )
                     + ")" ).c_str() );
            check( gotOff == wantOff,
                   ( "arp: every note-on is paired with one note-off ("
                     + std::to_string( wantOff ) + ", got " + std::to_string( gotOff )
                     + ")" ).c_str() );
            // The events come back with the key that was held and a host-visible
            // id, which is what makes them routable later.
            bool keysOk = true;
            for( const twEvent &e : t.out )
                if( e.key != kKeyC4 ) keysOk = false;
            check( keysOk, "arp: every emitted note carries the held key" );
        }
    }

    // --- AC2 (CLAP): a parameter step at EXACTLY the event's frame ----------
    {
        twPluginDescriptor d;
        d.format = "clap";
        d.uid    = "tw.test.clap.gain";
        d.path   = TW_TESTCLAP_PATH;
        std::unique_ptr<twPlugin> p = registry.instantiate( d );
        if( check( p != nullptr, "the CLAP gain effect instantiates" ) ) {
            p->prepare( kRate, kBlock );

            std::vector<float> inA( kBlock, 1.0f ), inB( kBlock, 1.0f );
            std::vector<float> outA( kBlock, 0.0f ), outB( kBlock, 0.0f );
            const float *ins[2]  = { inA.data(), inB.data() };
            float       *outs[2] = { outA.data(), outB.data() };
            float *const *outBuses[1] = { outs };

            twEvent            step = paramValue( 1234, 0, 0.5 );
            twEventList        list;
            list.events = &step;
            list.count  = 1;
            twEventOut         sink;
            twProcessContext   ctx;

            p->process( ins, outBuses, kBlock, list, sink, ctx );

            check( nearly( outA[1233], 1.0, 1e-6 ),
                   "clap: the frame BEFORE the parameter event is still at unity" );
            check( nearly( outA[1234], 0.5, 1e-6 ),
                   "clap: the step lands at EXACTLY frame 1234" );
            check( nearly( outA[kBlock - 1], 0.5, 1e-6 ),
                   "clap: ...and holds to the end of the block" );

            // The clipper (the fixture P3a's ORDER case needs). Gain 2 on a
            // constant 1.0 is 2.0; a threshold of 0.5 hard-clips it AFTER the
            // gain, so the result is the threshold, not 0.5 * input.
            p->setParam( 0, 2.0 );
            p->setParam( 2, 0.5 );
            twEventList none;
            p->process( ins, outBuses, kBlock, none, sink, ctx );
            check( nearly( outA[10], 0.5, 1e-6 ),
                   "clap: clipThreshold hard-clips AFTER the gain (2.0 -> 0.5)" );
            p->setParam( 2, 0.0 );
            p->process( ins, outBuses, kBlock, none, sink, ctx );
            check( nearly( outA[10], 2.0, 1e-6 ),
                   "clap: clipThreshold 0 is OFF, so nothing existing changes" );
        }
    }
#else
    std::cout << "  note CLAP fixtures SKIPPED (built without TW_HAVE_CLAP)" << std::endl;
#endif

#ifdef TW_TESTVST3_PATH
    // --- AC1/AC5 (c) + AC3: the SPLIT VST3 instrument ----------------------
    {
        const std::vector<twPluginDescriptor> all = vst3ModuleDescriptors( TW_TESTVST3_PATH );
        const twPluginDescriptor *sine = nullptr;
        for( const twPluginDescriptor &d : all )
            if( d.name == "TW Test VST3 Sine" ) sine = &d;

        if( check( sine != nullptr, "the VST3 module offers TW Test VST3 Sine" ) ) {
            std::unique_ptr<twPlugin> p = registry.instantiate( *sine );
            if( check( p != nullptr, "the SPLIT VST3 instrument instantiates" ) ) {
                const twPluginCapabilities c = p->capabilities();
                check( c.acceptsNotes && c.notePortsIn == 1,
                       "vst3: the kEvent input bus is discovered" );
                check( c.supportsNoteIds, "vst3: notes carry note ids" );
                check( p->ioLayout().audioOutputs == 2, "vst3: stereo out" );
                checkInstrument( *p, "vst3 sine", /*sineLike=*/true );
            }

            // AC3 — THE TEETH. The same fixture, down a deliberately BROKEN host
            // path: with SMARAGD_VST3_NO_EVENT_BUS=1 the backend does not call
            // activateBus(kEvent, kInput, 0, true), and the plugin — as the spec
            // entitles it to — ignores every note. If this render were NOT
            // silent, the assertion above would be proving nothing.
#if defined( _WIN32 )
            _putenv( (char *)"SMARAGD_VST3_NO_EVENT_BUS=1" );
#else
            setenv( "SMARAGD_VST3_NO_EVENT_BUS", "1", 1 );
#endif
            std::unique_ptr<twPlugin> broken = registry.instantiate( *sine );
            if( check( broken != nullptr, "a second instance for the broken path" ) ) {
                broken->prepare( kRate, kBlock );
                broken->reset();
                const std::vector<twEvent> one = { noteOn( 1000, kKeyC4, kVel100, 3 ) };
                const Take t = render( *broken, one, kBlock * 4 );
                check( peakIn( t.mono, 0, t.mono.size() ) < 1e-6,
                       "vst3: WITHOUT activateBus(kEvent) the instrument is SILENT "
                       "- so the note assertion above has teeth" );
            }
#if defined( _WIN32 )
            _putenv( (char *)"SMARAGD_VST3_NO_EVENT_BUS=" );
#else
            unsetenv( "SMARAGD_VST3_NO_EVENT_BUS" );
#endif
        }
    }

    // --- AC2 (VST3): a parameter point at EXACTLY its sampleOffset ---------
    //
    // The fixture IGNORES setParamNormalized (CONTRACT invariant 22), so only
    // an inputParameterChanges point with the right sampleOffset can move this
    // level. A host that wrote the controller, or that collapsed every point to
    // offset 0, fails here.
    {
        const std::vector<twPluginDescriptor> all = vst3ModuleDescriptors( TW_TESTVST3_PATH );
        const twPluginDescriptor *gain = nullptr;
        for( const twPluginDescriptor &d : all )
            if( d.name == "TW Test VST3 Gain" ) gain = &d;

        if( gain ) {
            std::unique_ptr<twPlugin> p = registry.instantiate( *gain );
            if( check( p != nullptr, "the VST3 gain effect instantiates" ) ) {
                p->prepare( kRate, kBlock );

                std::vector<float> inA( kBlock, 1.0f ), inB( kBlock, 1.0f );
                std::vector<float> outA( kBlock, 0.0f ), outB( kBlock, 0.0f );
                const float *ins[2]  = { inA.data(), inB.data() };
                float       *outs[2] = { outA.data(), outB.data() };
                float *const *outBuses[1] = { outs };

                twEvent          step = paramValue( 1234, 0, 0.5 );
                twEventList      list;
                list.events = &step;
                list.count  = 1;
                twEventOut       sink;
                twProcessContext ctx;

                p->process( ins, outBuses, kBlock, list, sink, ctx );
                check( nearly( outA[1233], 1.0, 1e-6 ),
                       "vst3: the frame BEFORE the parameter point is at unity" );
                check( nearly( outA[1234], 0.5, 1e-6 ),
                       "vst3: the step lands at EXACTLY frame 1234" );
                check( nearly( outA[kBlock - 1], 0.5, 1e-6 ),
                       "vst3: ...and holds to the end of the block" );
            }
        }
    }
#else
    std::cout << "  note VST3 fixtures SKIPPED (built without TW_HAVE_VST3)" << std::endl;
#endif

#ifndef __APPLE__
    // AC1 asks for AU too. AudioUnit hosting is macOS-only (TW_HAVE_AU), so on
    // this platform there is no AU to drive and NOTHING here was verified: the
    // AU event path (aumu/aumi enumeration, MusicDeviceMIDIEvent before the
    // render, AudioUnitScheduleParameters, the output elements) is written to
    // the documented API and has never been compiled, let alone run. Said out
    // loud rather than left as a silent gap.
    std::cout << "  note AU instrument SKIPPED: AudioUnit hosting is macOS-only, and "
                 "this build is not macOS. The AU event path is UNVERIFIED."
              << std::endl;
#endif

    return 0;
}

int testPluginInsert()
{
    testBuiltinPlugin();
    testChannelPolicy();
    testMissingAndReload();
    testChainAudio();
    testConcurrentTapFreeze();
    testEventAbi();
#ifdef TW_TESTCLAP_PATH
    testClapBackend();
#else
    std::cout << "=== CLAP backend: SKIPPED (built without TW_HAVE_CLAP) ===" << std::endl;
#endif
#ifdef TW_TESTVST3_PATH
    testVst3Backend();
#else
    std::cout << "=== VST3 backend: SKIPPED (built without TW_HAVE_VST3) ===" << std::endl;
#endif

    if( gFailures ) {
        std::cerr << "=== " << gFailures << " check(s) failed ===" << std::endl;
        return 1;
    }
    std::cout << "=== All tests passed ===" << std::endl;
    return 0;
}

}  // namespace audio

// Entry point for standalone test (if invoked directly).
#if defined(TEST_PLUGIN_INSERT_MAIN)
int main() {
    return audio::testPluginInsert();
}
#endif
