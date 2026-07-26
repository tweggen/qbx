#include "tw/plugins/twplugininsert.h"
#include "tw/graph/tw303aenv.h"
#include "tw/pages/io_vector.h"
#include "tw/plugins/twplugindescriptor.h"
#include <cmath>
#include <cstring>
#include <iostream>
#include <vector>

namespace audio {

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

    std::unique_ptr<twPlugin> plugin = registry.instantiate( plugins[0] );
    if( !check( plugin != nullptr, "instantiate PassThrough" ) )
        return 1;

    auto insert = std::make_unique<twPluginInsert>( env, std::move( plugin ) );

    check( insert->getNInputs() == 2 && insert->getNOutputs() == 2,
           "PassThrough is 2-in / 2-out" );
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

    return 0;
}

#endif  // TW_TESTCLAP_PATH

int testPluginInsert()
{
    testBuiltinPlugin();
#ifdef TW_TESTCLAP_PATH
    testClapBackend();
#else
    std::cout << "=== CLAP backend: SKIPPED (built without TW_HAVE_CLAP) ===" << std::endl;
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
