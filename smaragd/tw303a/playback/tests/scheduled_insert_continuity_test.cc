// A STATEFUL INSERT, DRIVEN BY THE DATAFLOW SCHEDULER, IS RENDERED ONCE PER PAGE.
//
// The shape is a track's (or the master lane's) post-mix chain: source ->
// twPluginChain -> twPluginInsert x2, demanded through CaptureRevalidator the
// way AudioEngine's readahead demands the root for an export and for playback.
//
// What it guards, measured on a user project (master chain NassauEQ -> Castello
// Reverb -> Mangrove -> NassauEQ, 2026-10-10): every page of every master slot
// was rendered TWICE, and the second render reset the plugin -- a click and a
// ~4 dB dip at nearly every 65536-frame boundary of the export. twPluginChain
// inherits the base planPage(), which declares its INPUT plug's producer, but
// its freezePage() renders by asking its LAST INSERT for the page. The insert is
// not in the plan, so the chain node records bound-set misses, verify-at-publish
// retries the node (capture_revalidator.cc, processGraphNode), the retry
// invalidates the chain's range -- which twPluginChain forwards to its inserts --
// and renders page P again, through processors whose lastEnd_ is already
// P + 65536. Each slot reads that as a reposition and resets its plugin.
//
// MockPlugin::reset() is the observable a real reverb would HEAR. A contiguous
// run from 0 must never reach it; a complete plan records no misses; and with
// neither a stale dependency nor a miss there is nothing to retry (schedule
// CONTRACT inv. 7).
//
// It lives under playback/ only because the layering allows a playback test to
// see both tw/schedule and tw/plugins; nothing here needs the engine itself.

#include "tw/plugins/twplugin.h"
#include "tw/plugins/twpluginchain.h"
#include "tw/plugins/twplugininsert.h"
#include "tw/plugins/twpluginslotproc.h"
#include "tw/schedule/capture_revalidator.h"
#include "tw/pages/capture_page_pool.h"
#include "tw/pages/tw_output_page.h"
#include "tw/graph/tw303aenv.h"
#include "tw/graph/twcomponent.h"

#include <atomic>
#include <cmath>
#include <iostream>
#include <memory>
#include <vector>

using namespace audio;

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

// out[c] = in[c] * (c + 1); counts reset() calls, which is the whole point.
class CountingPlugin : public twPlugin {
public:
    CountingPlugin() { io_.audioInputs = 2; io_.audioOutputs = 2; }

    const twPluginIoLayout &ioLayout() const override { return io_; }
    void prepare( std::uint32_t, std::uint32_t ) override {}
    void process( const float *const *in, float *const *out,
                  std::uint32_t nframes ) override
    {
        for( std::uint16_t c = 0; c < io_.audioOutputs; ++c )
            for( std::uint32_t i = 0; i < nframes; ++i )
                out[c][i] = in[c][i] * (float)( c + 1 );
    }
    void reset() override { resets.fetch_add( 1 ); }

    std::size_t       paramCount() const override { return 0; }
    twPluginParamInfo paramInfo( std::size_t ) const override { return {}; }
    double            getParam( std::uint32_t ) const override { return 0.0; }
    void              setParam( std::uint32_t, double ) override {}
    std::vector<std::uint8_t> saveState() const override { return {}; }
    bool loadState( const std::vector<std::uint8_t> & ) override { return true; }

    std::atomic<int> resets{ 0 };

private:
    twPluginIoLayout io_{};
};

// Position-deterministic wide source: value(c, p) depends only on channel and
// absolute frame, so any page served for the wrong position shows.
class TestSource : public twComponent {
public:
    explicit TestSource( tw303aEnvironment &env ) : twComponent( env ) {}

    idx_t getNInputs() const override  { return 0; }
    idx_t getNOutputs() const override { return 1; }
    idx_t getOutputChannels() const override { return 2; }
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
    bool usesSerialCursor() const override { return true; }
    int  seekTo( offset_t o ) override { pos_ = o; return 0; }

    length_t renderPageWide( twOutputPage &page, length_t frames,
                             const sample_t *, length_t ) override
    {
        length_t n = frames;
        if( n > (length_t)page.channelFrames() ) n = (length_t)page.channelFrames();
        for( idx_t c = 0; c < (idx_t)page.channels(); ++c ) {
            sample_t *dst = page.channelPtr( c );
            for( length_t i = 0; i < n; ++i ) dst[i] = value( c, pos_ + i );
        }
        pos_ += n;
        return n;
    }

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
    offset_t pos_ = 0;
};

struct Slot {
    std::shared_ptr<twPluginSlotProcessor> proc;
    std::shared_ptr<twPluginInsert>        insert;
    CountingPlugin                        *plugin = nullptr;
};

Slot makeSlot( tw303aEnvironment &env )
{
    Slot s;
    CountingPlugin **made = new CountingPlugin *( nullptr );
    s.proc = std::make_shared<twPluginSlotProcessor>(
        env,
        [made]() -> std::unique_ptr<twPlugin> {
            auto p = std::make_unique<CountingPlugin>();
            *made = p.get();
            return p;
        },
        twPluginIoLayout{ 2, 2 } );
    s.proc->setChannelCount( 2 );
    s.plugin = *made;
    delete made;
    s.insert = std::make_shared<twPluginInsert>( env, s.proc );
    s.insert->init();
    return s;
}

}  // namespace

int main()
{
    std::cout << "=== scheduled chain: a stateful insert renders each page once ===" << std::endl;

    tw303aEnvironment env;
    env.setSRate( 48000 );
    const int      rate   = env.getSRate();
    const length_t pageN  = (length_t)twOutputPage::FRAME_CAPACITY;
    const int      nPages = 4;

    auto source = std::make_shared<TestSource>( env );
    source->init();

    // Two inserts, as a real master chain has several: the first is fed by the
    // chain's input, the second by the first (twPluginChain::rebuildWiring).
    Slot a = makeSlot( env );
    Slot b = makeSlot( env );
    if( !check( a.plugin && b.plugin, "each slot built its plugin instance" ) )
        return 1;

    auto chain = std::make_shared<twPluginChain>( env, 2 );
    chain->init();
    chain->setInput( 0, source->linkOutput( 0 ) );
    chain->addPlugin( a.insert );
    chain->addPlugin( b.insert );

    {
        CapturePagePool    pool( 16 );
        CaptureRevalidator reval( &pool, 2 );
        auto d = reval.requestGraphPages( chain, 0, nPages );
        d->wait();

        const auto st = reval.graphStats();
        std::cout << "  info graph stats: nodesExecuted=" << st.nodesExecuted
                  << " nodeRetries=" << st.nodeRetries
                  << " missPages=" << st.missPages
                  << " selfStale=" << st.selfStale
                  << "; plugin resets first=" << a.plugin->resets.load()
                  << " last=" << b.plugin->resets.load() << std::endl;

        check( st.missPages == 0,
               "the chain's plan is complete: no bound-set misses" );
        check( st.nodeRetries == 0,
               "so verify-at-publish retries nothing" );
        check( a.plugin->resets.load() == 0,
               "a contiguous scheduled run never resets the FIRST insert's plugin" );
        check( b.plugin->resets.load() == 0,
               "...nor the LAST insert's (the one the chain asks for its page)" );

        // The audio is the contiguous render: each insert multiplies channel c
        // by (c + 1), so after two of them channel 1 carries 4x its input.
        bool ok = true;
        for( int p = 0; p < nPages && ok; ++p ) {
            const offset_t pos = (offset_t)p * (offset_t)pageN;
            auto page = chain->requestPage( pos, nullptr, 0, pageN, rate, nullptr );
            if( !page || page->validFrames != (uint32_t)pageN ) { ok = false; break; }
            for( length_t i = 0; i < 64 && ok; ++i ) {
                ok = nearly( page->channelPtr( 0 )[i], source->value( 0, pos + i ) )
                  && nearly( page->channelPtr( 1 )[i], source->value( 1, pos + i ) * 4.0f );
            }
        }
        check( ok, "every scheduled page carries the audio for its own position" );
    }

    chain->teardown();

    if( gFailures ) {
        std::cerr << "=== " << gFailures << " check(s) failed ===" << std::endl;
        return 1;
    }
    std::cout << "=== All tests passed ===" << std::endl;
    return 0;
}
