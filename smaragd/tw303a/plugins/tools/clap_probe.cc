// clap_probe — de-risking spike for proposal 08 M0.
//
// Loads one .clap module with the production loader (twClapModule) and prints
// what its factory offers. Its only job is to answer, before any engine work is
// committed: can a binary built by Qt's bundled MinGW g++ load a real CLAP
// plugin — typically an MSVC-built DLL — resolve clap_entry, enumerate the
// factory, instantiate a plugin and read its port/parameter layout?
//
// NOT shipped, and not a test gate. It lives under plugins/tools/ because that
// is the established home for engine dev tools (cf. analysis/tools/warp_ab.cc)
// and because tools/ is an ALLOW_DIR in tools/check_logging.py: printing a table
// to stdout IS this program's output, not a diagnostic.
//
//   clap_probe <path-to-plugin.clap> [more.clap ...]
//
// Exit code 0 if every argument yielded at least one descriptor, 1 otherwise.

#include "twclapmodule.h"

#include "tw/core/twlog.h"

#include <cstdio>
#include <string>
#include <vector>

#if defined( _WIN32 )
#include <windows.h>
#endif

namespace {

int probeOne( const std::string &path )
{
    std::printf( "=== %s\n", path.c_str() );

    std::shared_ptr<audio::twClapModule> mod = audio::twClapModule::open( path );
    if( !mod ) {
        std::printf( "  FAILED to load (see the log above for the reason)\n" );
        return 1;
    }

    const std::vector<audio::twPluginDescriptor> descs =
        audio::clapModuleDescriptors( path );
    if( descs.empty() ) {
        std::printf( "  loaded, but the factory offered no usable descriptor\n" );
        return 1;
    }

    std::printf( "  %zu plugin(s)\n", descs.size() );
    for( const audio::twPluginDescriptor &d : descs ) {
        std::printf( "  - uid    : %s\n", d.uid.c_str() );
        std::printf( "    name   : %s\n", d.name.c_str() );
        std::printf( "    vendor : %s\n", d.vendor.c_str() );
        std::printf( "    io     : %u in / %u out%s\n",
                     (unsigned)d.io.audioInputs, (unsigned)d.io.audioOutputs,
                     d.isInstrument ? "  [instrument]" : "" );

        std::unique_ptr<audio::twPlugin> inst = audio::createClapPlugin( d.path, d.uid );
        if( !inst ) {
            std::printf( "    instantiate: FAILED\n" );
            continue;
        }
        std::printf( "    latency: %u frames, native editor: %s, notes: %s\n",
                     (unsigned)inst->reportedLatency(),
                     inst->supportsNativeEditor() ? "yes" : "no",
                     inst->acceptsNotes() ? "yes" : "no" );
        std::printf( "    params : %zu\n", inst->paramCount() );
        for( std::size_t i = 0; i < inst->paramCount(); ++i ) {
            const audio::twPluginParamInfo pi = inst->paramInfo( i );
            std::printf( "      [%u] %-32s %g .. %g (default %g)%s\n",
                         (unsigned)pi.id, pi.name.c_str(), pi.minValue, pi.maxValue,
                         pi.defaultValue, pi.isStepped ? "  stepped" : "" );
        }

        // Exercise the paths the engine will use, so a plugin that loads but
        // cannot be activated is caught here rather than at first render.
        inst->prepare( 48000, 4096 );
        const std::vector<std::uint8_t> state = inst->saveState();
        std::printf( "    state  : %zu bytes, reloads: %s\n", state.size(),
                     inst->loadState( state ) ? "yes" : "no" );
    }

    return 0;
}

}  // namespace

int main( int argc, char **argv )
{
#if defined( _WIN32 )
    // Never answer a malformed module with a modal dialog — see the same call in
    // tools/plugin_probe.cc. A spike run over a directory of unknown files must
    // stay unattended.
    SetErrorMode( SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX
                  | SEM_NOOPENFILEERRORBOX );
#endif

    if( argc < 2 ) {
        std::printf( "usage: %s <plugin.clap> [more.clap ...]\n",
                     argc > 0 ? argv[0] : "clap_probe" );
        return 2;
    }

    // Mirror the log to stderr so a load failure's reason is visible next to the
    // table; without this the TW_LOG* lines go only to the ring buffer.
    tw::TwLog::instance().setConsole( true );
    tw::TwLog::instance().setMinLevel( tw::LogLevel::Debug );

    int rc = 0;
    for( int i = 1; i < argc; ++i )
        rc |= probeOne( argv[i] );
    return rc;
}
