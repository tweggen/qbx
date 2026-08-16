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

        // AC B4.4: how many of this plugin's inputs carried actual SIGNAL in
        // this one call. Counted on the way IN, because two silent channels are
        // indistinguishable from "the host only wired one" once they have been
        // multiplied by a gain and written out.
        {
            std::uint16_t liveIn = 0;
            for( std::uint16_t c = 0; c < io_.audioInputs; ++c ) {
                bool any = false;
                for( std::uint32_t i = 0; i < nframes && !any; ++i )
                    any = ( in[c][i] != 0.0f );
                if( any ) ++liveIn;
            }
            if( liveIn > maxChannelsSeen_ ) maxChannelsSeen_ = liveIn;
        }

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
    std::uint16_t maxChannelsSeen_ = 0;   // see process(); AC B4.4
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

// A position-deterministic WIDE source: value(c, p) depends only on the channel
// and the absolute frame position, so a page's contents can be predicted
// exactly and a page served for the wrong position — or a channel filled from
// the wrong page, the "coherent page displaced by one page" bug — is
// detectable.
//
// Proposal 36 B4: this used to be N separate MONO sources, one per bus, because
// a page was mono and a track was N parallel component instances. One wide
// source with an asymmetric per-channel signal is the same test at the shape
// the engine now has, and value(c, p) reproduces exactly what the old
// `TestSource(base = c+1)->value(p)` produced for bus c.
class TestSource : public twComponent {
public:
    TestSource( tw303aEnvironment &env, idx_t channels )
        : twComponent( env ), channels_( channels < 1 ? 1 : channels ) {}

    idx_t getNInputs() const override  { return 0; }
    idx_t getNOutputs() const override { return 1; }
    idx_t getOutputChannels() const override { return channels_; }
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

    // §4.3's shape: seek once, fill every channel in one pass, advance once.
    length_t renderPageWide( twOutputPage &page, length_t frames,
                             const sample_t *, length_t ) override
    {
        length_t n = frames;
        if( n > (length_t)page.channelFrames() ) n = (length_t)page.channelFrames();
        const idx_t nCh = (idx_t)page.channels();
        for( idx_t c = 0; c < nCh; ++c ) {
            sample_t *dst = page.channelPtr( c );
            for( length_t i = 0; i < n; ++i ) dst[i] = value( c, pos_ + i );
        }
        pos_ += n;
        return n;
    }

    // The narrow degradation (§7 trap 18): channel 0, and it must exist or the
    // base renderFrames/calcOutputTo pair recurses until the stack ends.
    length_t renderFrames( sample_t *out, length_t len, const sample_t *,
                           length_t, idx_t ) override
    {
        for( length_t i = 0; i < len; ++i ) out[i] = value( 0, pos_ + i );
        pos_ += len;
        return len;
    }

    float value( idx_t c, offset_t p ) const
    {
        return (float)( c + 1 ) * (float)( ( p % 17 ) + 1 ) * 0.01f;
    }

private:
    idx_t    channels_;
    offset_t pos_ = 0;
};

// Build a slot: one processor, one wide source, ONE insert (proposal 36 B4 —
// there used to be one tap per bus, each with its own mono source).
struct Rig {
    std::shared_ptr<twPluginSlotProcessor> proc;
    std::shared_ptr<TestSource>            source;
    std::shared_ptr<twPluginInsert>        insert;
};

Rig buildRig( tw303aEnvironment &env, int nChannels, int pluginIn, int pluginOut,
              std::atomic<int> *live = nullptr )
{
    Rig r;
    r.proc = std::make_shared<twPluginSlotProcessor>(
        env,
        [pluginIn, pluginOut, live]() -> std::unique_ptr<twPlugin> {
            return std::make_unique<MockPlugin>( pluginIn, pluginOut, live );
        },
        twPluginIoLayout{ (std::uint16_t)pluginIn, (std::uint16_t)pluginOut } );
    r.proc->setChannelCount( (idx_t)nChannels );

    r.source = std::make_shared<TestSource>( env, (idx_t)nChannels );
    r.source->init();
    r.insert = std::make_shared<twPluginInsert>( env, r.proc );
    r.insert->init();
    r.insert->setInput( 0, r.source->linkOutput( 0 ) );
    return r;
}

std::shared_ptr<twOutputPage> freezeInsert( const std::shared_ptr<twPluginInsert> &insert,
                                            offset_t pos, int rate )
{
    return insert->requestPage( pos, nullptr, 0,
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
//
// PROPOSAL 36 B4 PRESERVED THIS POLICY SEMANTICALLY AND CHANGED ONLY WHERE THE
// NUMBER COMES FROM: it is the PAGE WIDTH the slot's single insert is handed,
// not the count of parallel mono components a track was built out of. Every
// verdict below — Direct, DualMono with its instance count, MonoFold's average,
// Unsupported staying transparent — is the one proposal 08 settled, and the
// levels asserted are the same numbers the per-bus version asserted.
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
               "2-in/2-out on 2 channels is Direct" );
        check( r.proc->state() == twPluginSlotState::Active, "...and Active" );
        check( live.load() == 1, "...served by exactly ONE plugin instance" );
        check( r.insert->getOutputChannels() == 2,
               "...and the insert declares the slot's width" );
    }

    // --- 1 -> 1 on N channels: dual-mono ----------------------------------
    {
        std::atomic<int> live{ 0 };
        Rig r = buildRig( env, 2, 1, 1, &live );
        check( r.proc->mode() == twPluginSlotMode::DualMono,
               "1-in/1-out on 2 channels is DualMono" );
        check( live.load() == 2,
               "...served by ONE INSTANCE PER CHANNEL (why the processor takes a factory)" );

        // Prove the instances are genuinely independent: give channel 1's its
        // own gain and watch only channel 1 change.
        std::vector<twPlugin *> ps = r.proc->plugins();
        if( check( ps.size() == 2, "both dual-mono instances are reachable" ) ) {
            ps[1]->setParam( 0, 3.0 );
            r.proc->bumpParamEpoch();
            auto p = freezeInsert( r.insert, 0, rate );
            const float in0 = r.source->value( 0, 100 );
            const float in1 = r.source->value( 1, 100 );
            check( p && p->channels() == 2, "the slot's page is two channels wide" );
            check( p && nearly( p->channelPtr(0)[100], in0 * 1.0f, 1e-6 ),
                   "dual-mono channel 0 keeps its own gain" );
            check( p && nearly( p->channelPtr(1)[100], in1 * 3.0f, 1e-5 ),
                   "dual-mono channel 1 uses its own instance's gain" );
        }
    }

    // --- 2 -> 2 on ONE channel: feed both inputs, average the outputs -----
    {
        Rig r = buildRig( env, 1, 2, 2 );
        check( r.proc->mode() == twPluginSlotMode::MonoFold,
               "2-in/2-out on 1 channel is MonoFold" );
        auto p = freezeInsert( r.insert, 0, rate );
        // MockPlugin scales channel c by (c+1), and both inputs see the same
        // mono wire, so the average of the two outputs is in * 1.5.
        const float in = r.source->value( 0, 77 );
        check( p && nearly( p->channelPtr(0)[77], in * 1.5f, 1e-5 ),
               "MonoFold feeds both inputs and averages the outputs" );
    }

    // --- anything else: Unsupported, transparent, logged once -------------
    {
        Rig r = buildRig( env, 2, 3, 3 );
        check( r.proc->mode() == twPluginSlotMode::Transparent,
               "3-in/3-out on 2 channels has no mapping" );
        check( r.proc->state() == twPluginSlotState::Unsupported,
               "...and the slot is Unsupported" );
        auto p = freezeInsert( r.insert, 0, rate );
        check( p && nearly( p->channelPtr(0)[5], r.source->value( 0, 5 ), 1e-6 ) &&
                   nearly( p->channelPtr(1)[5], r.source->value( 1, 5 ), 1e-6 ),
               "...and it loads TRANSPARENT (input reaches the output unchanged, "
               "on every channel)" );
    }
    {
        Rig r = buildRig( env, 1, 1, 2 );
        check( r.proc->state() == twPluginSlotState::Unsupported,
               "an asymmetric 1-in/2-out plugin is Unsupported" );
    }

    // --- AC B4.4: the table still holds at width 6 ------------------------
    //
    // Six is the width nothing in this repo had ever run a plugin at, and it is
    // where "the mapping is derived from the page width" earns its keep: a 2->2
    // plugin that is Direct on a stereo track must be UNSUPPORTED here, and a
    // 1->1 plugin must produce six independent instances.
    {
        std::atomic<int> live{ 0 };
        Rig r = buildRig( env, 6, 6, 6, &live );
        check( r.proc->mode() == twPluginSlotMode::Direct,
               "6-in/6-out on 6 channels is Direct" );
        check( live.load() == 1, "...one instance" );
        auto p = freezeInsert( r.insert, 0, rate );
        bool ok = p && p->channels() == 6;
        for( idx_t c = 0; ok && c < 6; ++c ) {
            ok = nearly( p->channelPtr(c)[13],
                         r.source->value( c, 13 ) * (float)( c + 1 ), 1e-5 );
        }
        check( ok, "...and all six channels went through the plugin coherently" );
    }
    {
        std::atomic<int> live{ 0 };
        Rig r = buildRig( env, 6, 1, 1, &live );
        check( r.proc->mode() == twPluginSlotMode::DualMono,
               "1-in/1-out on 6 channels is DualMono" );
        check( live.load() == 6, "...with six independent instances" );
    }
    {
        Rig r = buildRig( env, 6, 2, 2 );
        check( r.proc->mode() == twPluginSlotMode::Transparent &&
               r.proc->state() == twPluginSlotState::Unsupported,
               "2-in/2-out on 6 channels is Unsupported (no routing matrix — "
               "proposal 36 §8 non-goal)" );
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
// — the insert and the twPluginChain holding it are never rebuilt, because a
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
    proc->setChannelCount( 2 );

    auto source = std::make_shared<TestSource>( env, 2 );
    source->init();
    auto insert = std::make_shared<twPluginInsert>( env, proc );
    insert->init();
    insert->setInput( 0, source->linkOutput( 0 ) );

    check( proc->state() == twPluginSlotState::Missing,
           "a factory that produces nothing leaves the slot MISSING" );
    check( proc->mode() == twPluginSlotMode::Direct,
           "...but the mapping is still Direct, derived from the DECLARED 2-in/2-out "
           "(so a reload does not change the graph's shape)" );
    check( live.load() == 0, "...and no real plugin instance exists" );
    {
        auto p = freezeInsert( insert, 0, rate );
        check( p && nearly( p->channelPtr(0)[9], source->value( 0, 9 ), 1e-6 ) &&
                   nearly( p->channelPtr(1)[9], source->value( 1, 9 ), 1e-6 ),
               "...the placeholder is bit-transparent on every channel" );
    }

    // The rescan found it. Same processor, new factory.
    installed.store( true );
    proc->setFactory( [&installed, &live]() -> std::unique_ptr<twPlugin> {
        if( !installed.load() ) return nullptr;
        return std::make_unique<MockPlugin>( 2, 2, &live );
    } );

    check( proc->state() == twPluginSlotState::Active,
           "setFactory() turns a MISSING slot Active without touching the insert" );
    check( live.load() == 1, "...with exactly one real instance for Direct" );
    {
        // MockPlugin scales channel c by gain * (c + 1) — so channel 1 at 2x is
        // the proof the insert still reaches the SAME processor after the swap.
        auto p = freezeInsert( insert, 0, rate );
        check( p && nearly( p->channelPtr(0)[9], source->value( 0, 9 ) * 1.0f, 1e-6 ),
               "...channel 0 is now processed" );
        check( p && nearly( p->channelPtr(1)[9], source->value( 1, 9 ) * 2.0f, 1e-5 ),
               "...channel 1 too, through the same insert as before" );
    }

    // A state chunk re-applied after the reload has to be audible, which is what
    // makes "the settings survived the plugin being missing" true rather than
    // merely stored. (The app does exactly this in SPluginSlot::reloadPlugin.)
    for( twPlugin *p : proc->plugins() ) p->setParam( 0, 4.0 );
    proc->bumpParamEpoch();
    {
        auto p = freezeInsert( insert, 0, rate );
        check( p && nearly( p->channelPtr(0)[9], source->value( 0, 9 ) * 4.0f, 1e-5 ),
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
        auto p = freezeInsert( insert, 0, rate );
        check( p && nearly( p->channelPtr(0)[9], source->value( 0, 9 ), 1e-6 ),
               "...transparent once more" );
    }

    // A declared layout with no mapping AND no plugin: MISSING wins over
    // Unsupported, because "the plugin is not here" is the actionable report.
    {
        auto p = std::make_shared<twPluginSlotProcessor>(
            env, []() -> std::unique_ptr<twPlugin> { return nullptr; },
            twPluginIoLayout{ 3, 3 } );
        p->setChannelCount( 2 );
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
// read as mono, and a 2-in plugin's second input was never wired at all. Since
// proposal 36 B4 it is ONE page, genuinely two channels wide, which is the
// shape M3's comment was describing as impossible.
static int testChainAudio()
{
    std::cout << "=== audio through a two-channel slot ===" << std::endl;

    tw303aEnvironment env;
    env.setSRate( 48000 );
    const int      rate  = env.getSRate();
    const length_t pageN = (length_t)twOutputPage::FRAME_CAPACITY;

    Rig r = buildRig( env, 2, 2, 2 );

    auto p0 = freezeInsert( r.insert, 0, rate );
    if( !check( p0 != nullptr, "the insert produced a page" ) ) return 1;

    check( p0->channels() == 2, "...two channels wide" );
    check( p0->validFrames == (uint32_t)pageN, "...and a full page of frames" );

    // MockPlugin: out[c] = in[c] * (c+1), and the source's channel c carries
    // (c+1) * f(pos) — so a silent second input, a swapped pair or an
    // interleaved write all show up here.
    bool okL = true, okR = true, differ = false;
    for( length_t i = 0; i < 4096; ++i ) {
        const float wantL = r.source->value( 0, i ) * 1.0f;
        const float wantR = r.source->value( 1, i ) * 2.0f;
        okL = okL && nearly( p0->channelPtr(0)[i], wantL, 1e-6 );
        okR = okR && nearly( p0->channelPtr(1)[i], wantR, 1e-6 );
        if( !nearly( p0->channelPtr(0)[i], p0->channelPtr(1)[i], 1e-9 ) ) differ = true;
    }
    check( okL, "channel 0 carries its own upstream through the plugin" );
    check( okR, "channel 1 carries ITS OWN upstream through the plugin (not silence)" );
    check( differ, "the two channels are genuinely different audio" );

    // Chunking (CONTRACT invariant 5): the plugin never sees more than it was
    // activated for, and it really does see the declared block size.
    std::vector<twPlugin *> ps = r.proc->plugins();
    if( check( ps.size() == 1, "Direct mode has one instance" ) ) {
        MockPlugin *mp = static_cast<MockPlugin *>( ps[0] );
        check( mp->overruns_ == 0, "the plugin was never handed more than maxBlock" );
        check( mp->maxSeen_ == (std::uint32_t)twPluginSlotProcessor::kChunkFrames,
               "a 65536-frame page reaches the plugin as kChunkFrames blocks" );
        check( mp->prepares_ == 1, "prepare() ran exactly once for the slot" );
        check( mp->maxChannelsSeen_ == 2,
               "AC B4.4: the plugin saw BOTH channels in one process() call" );
    }

    // A second page at the same position must be served from the cache, not
    // re-rendered.
    auto p0b = freezeInsert( r.insert, 0, rate );
    check( p0b == p0, "re-requesting the same page hits the component cache" );

    // The next page in sequence must not reset the plugin (state continuity).
    auto n0 = freezeInsert( r.insert, (offset_t)pageN, rate );
    if( check( n0 != nullptr, "the following page freezes" ) ) {
        check( nearly( n0->channelPtr(0)[3], r.source->value( 0, (offset_t)pageN + 3 ), 1e-6 ),
               "the following page carries the audio for ITS position" );
        check( nearly( n0->channelPtr(1)[3],
                       r.source->value( 1, (offset_t)pageN + 3 ) * 2.0f, 1e-6 ),
               "...on channel 1 as well, i.e. not displaced by a page (§4.3)" );
    }

    // --- bypass must invalidate pages, or the toggle is inaudible ----------
    r.proc->setBypass( true );
    auto b0 = freezeInsert( r.insert, 0, rate );
    check( b0 != p0, "toggling bypass stales the cached page" );
    check( b0 && nearly( b0->channelPtr(0)[9], r.source->value( 0, 9 ), 1e-6 ) &&
               nearly( b0->channelPtr(1)[9], r.source->value( 1, 9 ), 1e-6 ),
           "a bypassed slot passes its input straight through, on every channel" );
    r.proc->setBypass( false );
    auto u0 = freezeInsert( r.insert, 0, rate );
    check( u0 && nearly( u0->channelPtr(0)[9], r.source->value( 0, 9 ) * 1.0f, 1e-6 ),
           "un-bypassing brings the plugin back" );

    // --- a parameter edit must invalidate pages too ------------------------
    if( !ps.empty() ) {
        ps[0]->setParam( 0, 4.0 );
        r.proc->bumpParamEpoch();
        auto g0 = freezeInsert( r.insert, 0, rate );
        check( g0 && nearly( g0->channelPtr(0)[9], r.source->value( 0, 9 ) * 4.0f, 1e-5 ),
               "a parameter edit is audible on the next freeze (pages invalidated)" );
    }

    // --- a preview freeze must not touch the plugin ------------------------
    if( !ps.empty() ) {
        MockPlugin *mp = static_cast<MockPlugin *>( ps[0] );
        const int preparesBefore = mp->prepares_;
        auto pv = r.insert->freezePage( 0, nullptr, 0, 1000, 1000, nullptr );
        check( mp->prepares_ == preparesBefore,
               "a preview freeze does NOT re-prepare the plugin (CONTRACT 6)" );
        check( pv && nearly( pv->channelPtr(0)[3], r.source->value( 0, 3 ), 1e-6 ),
               "...and forwards the upstream envelope unprocessed" );
    }

    return 0;
}

// ---------------------------------------------------------------------------
// CONCURRENT FREEZES OF ONE SLOT.
//
// What this gate used to be, and why it changed. Proposal 08 M3's hard
// invariant 1 was about two TAPS of one slot freezing at once: tap 0 rendered
// holding the processor mutex and gathered bus 1 SIDEWAYS through tap 1's
// pullUpstreamPage(), so a tap that held its own component mutex there
// deadlocked against tap 1's own freeze. Proposal 36 B4 retired the sideways
// gather — there is one insert, it reads its own upstream page, and no lock is
// ever taken across a sibling — so that particular deadlock is not reachable
// any more.
//
// What IS still reachable, and is what this now gates, is the other hazard the
// same slot has: two drivers (a revalidation worker, the playback readahead, an
// offline render) freezing the SAME insert at DIFFERENT positions at the same
// time. The insert has a streaming input, so its freeze serializes on
// cursorMutex_ (twComponent::freezePage), and the processor serializes its own
// state; if either failed, one thread's page would come back carrying the other
// thread's position — the "coherent page displaced by a whole page" bug this
// repo has already bled for. A hang is reported as a failure and the process is
// aborted, because a deadlocked thread cannot be joined.
static int testConcurrentSlotFreeze()
{
    std::cout << "=== concurrent freezes of one slot (race + deadlock gate) ==="
              << std::endl;

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
            // Force a real render this round.
            r.proc->bumpParamEpoch();
            const offset_t posA = (offset_t)( ( it % 3 ) * pageN );
            const offset_t posB = (offset_t)( ( ( it + 1 ) % 3 ) * pageN );
            const offset_t pos[2] = { posA, posB };

            std::atomic<int> ready{ 0 };
            std::shared_ptr<twOutputPage> pages[2];

            auto worker = [&]( int which ) {
                ready.fetch_add( 1 );
                while( ready.load() < 2 ) { /* line the two threads up */ }
                pages[which] = freezeInsert( r.insert, pos[which], rate );
            };

            std::thread t0( worker, 0 );
            std::thread t1( worker, 1 );
            t0.join();
            t1.join();

            for( int which = 0; which < 2; ++which ) {
                if( !pages[which] ) { mismatches.fetch_add( 1 ); continue; }
                if( pages[which]->channels() != 2 ) { mismatches.fetch_add( 1 ); continue; }
                for( idx_t c = 0; c < 2; ++c ) {
                    const float want =
                        r.source->value( c, pos[which] + 11 ) * (float)( c + 1 );
                    if( !nearly( pages[which]->channelPtr(c)[11], want, 1e-5 ) )
                        mismatches.fetch_add( 1 );
                }
            }
        }
        done.store( true );
    };

    std::future<void> fut = std::async( std::launch::async, body );
    if( fut.wait_for( std::chrono::seconds( 60 ) ) != std::future_status::ready ) {
        check( false, "concurrent slot freezes complete without deadlocking" );
        std::cerr << "=== DEADLOCK: aborting (a hung thread cannot be joined) ==="
                  << std::endl;
        std::cerr.flush();
        std::cout.flush();
        std::_Exit( 1 );
    }
    fut.get();

    check( done.load(), "concurrent slot freezes complete without deadlocking" );
    check( mismatches.load() == 0,
           "every concurrently frozen page carries every channel at the right position" );

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
    check( plugin->paramCount() == 2, "clap.params reports 2 parameters" );
    check( plugin->paramInfo( 0 ).name == "Gain", "parameter 0 is named Gain" );
    check( plugin->paramInfo( 1 ).isStepped, "parameter 1 is stepped" );
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
    const std::vector<twPluginDescriptor> found = vst3ModuleDescriptors( TW_TESTVST3_PATH );
    if( check( found.size() == 1, "the module reports exactly one audio-effect class" ) ) {
        check( found[0].format == "vst3", "descriptor format is vst3" );
        check( found[0].uid.size() == 32, "uid is a 32-hex-digit class id" );
        check( found[0].name == "TW Test VST3 Gain", "descriptor carries the class name" );
        check( found[0].vendor == "Smaragd", "descriptor carries the vendor" );
        check( !found[0].isInstrument, "an Fx subcategory is not an instrument" );
        check( found[0].io.audioInputs == 2 && found[0].io.audioOutputs == 2,
               "descriptor I/O comes from a live instance" );

        twPluginDescriptor byUid = found[0];
        std::unique_ptr<twPlugin> resolved = registry.instantiate( byUid );
        check( resolved != nullptr, "a descriptor resolved by uid instantiates" );
    }

    // A path that is not a plugin must fail cleanly, not crash: this is the
    // property the out-of-process probe depends on for corrupt files.
    check( vst3ModuleDescriptors( "definitely-not-a-module.vst3" ).empty(),
           "a missing module yields no descriptors" );

    return 0;
}

#endif  // TW_TESTVST3_PATH

int testPluginInsert()
{
    testBuiltinPlugin();
    testChannelPolicy();
    testMissingAndReload();
    testChainAudio();
    testConcurrentSlotFreeze();
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
