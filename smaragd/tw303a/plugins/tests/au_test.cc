// au_test — the AudioUnit backend gate for proposal 08 M8 (macOS only).
//
// Built ONLY when the AU backend is compiled in (see tw303a/CMakeLists.txt), and
// it SKIPS (returns 0) when the machine registers no hostable AudioUnit — a
// stock-AU gate cannot be reproducible, so this asserts backend MECHANICS
// against whatever Apple effect is present, never bytes:
//   * the OS registry enumerates at least one hostable component
//   * a component instantiates as a twPlugin with a sane I/O layout
//   * process() runs a block without crashing
//   * the 'TWAU' state frame round-trips (save -> load returns true)
//   * setParam/getParam stay within the parameter's declared range
//
// Which AU is picked and what it computes varies by OS version, so there is no
// cmp/RMS assertion here — that discrimination lives in the qxa cases.

#include "tw/plugins/twplugin.h"

#ifdef TW_HAVE_AU
#include "twaumodule.h"
#endif

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

int gFailures = 0;

bool check( bool ok, const std::string &what )
{
    if( ok )
        std::cout << "  ok   " << what << std::endl;
    else {
        std::cerr << "  FAIL " << what << std::endl;
        ++gFailures;
    }
    return ok;
}

}  // namespace

int main()
{
#ifndef TW_HAVE_AU
    std::cout << "au_test: built without AU support; skipping" << std::endl;
    return 0;
#else
    using namespace audio;

    const std::vector<twPluginModuleFile> mods = enumerateAuModules();
    if( mods.empty() ) {
        std::cout << "au_test: no hostable AudioUnit registered on this machine; "
                     "skipping" << std::endl;
        return 0;
    }
    check( true, "the OS registry enumerates at least one hostable AU" );

    // First component that actually yields a descriptor.
    std::vector<twPluginDescriptor> descs;
    std::string                     uid;
    for( const twPluginModuleFile &m : mods ) {
        descs = auModuleDescriptors( m.path );
        if( !descs.empty() ) {
            uid = descs.front().uid;
            break;
        }
    }
    if( uid.empty() ) {
        std::cout << "au_test: no AU produced a descriptor; skipping" << std::endl;
        return 0;
    }
    check( descs.front().format == "au" && !descs.front().uid.empty(),
           "a component yields an 'au' descriptor with a uid" );
    std::cout << "  note  exercising AU '" << descs.front().name << "' (" << uid << ")"
              << std::endl;

    std::unique_ptr<twPlugin> p = createAuPlugin( "", uid );
    if( !check( p != nullptr, "the component instantiates as a twPlugin" ) )
        return 1;

    const twPluginIoLayout io = p->ioLayout();
    check( io.audioInputs > 0 && io.audioOutputs > 0,
           "the plugin reports a non-empty I/O layout" );

    const std::uint32_t sr = 48000, block = 512;
    p->prepare( sr, block );

    std::vector<std::vector<float>> in( io.audioInputs, std::vector<float>( block, 0.25f ) );
    std::vector<std::vector<float>> out( io.audioOutputs, std::vector<float>( block, 0.0f ) );
    std::vector<const float *>      inp( io.audioInputs );
    std::vector<float *>            outp( io.audioOutputs );
    for( std::uint16_t c = 0; c < io.audioInputs; ++c )  inp[c]  = in[c].data();
    for( std::uint16_t c = 0; c < io.audioOutputs; ++c ) outp[c] = out[c].data();
    p->process( inp.data(), outp.data(), block );
    check( true, "process() ran a block without crashing" );

    // Over-max block must pass through, not overrun the unit's buffers.
    p->process( inp.data(), outp.data(), block + 1 );
    check( true, "an over-max block passes through without crashing" );

    const std::vector<std::uint8_t> blob = p->saveState();
    check( blob.size() >= 8 && blob[0] == 'T' && blob[1] == 'W' && blob[2] == 'A'
               && blob[3] == 'U',
           "saveState() emits a TWAU-framed blob" );
    check( p->loadState( blob ), "loadState() accepts its own blob" );

    if( p->paramCount() > 0 ) {
        const twPluginParamInfo info = p->paramInfo( 0 );
        p->setParam( info.id, info.maxValue );
        const double got = p->getParam( info.id );
        const double lo  = std::min( info.minValue, info.maxValue ) - 1e-3;
        const double hi  = std::max( info.minValue, info.maxValue ) + 1e-3;
        check( got >= lo && got <= hi,
               "setParam/getParam stays within the parameter's declared range" );
    } else {
        std::cout << "  note  the picked AU exposes no parameters; skipping the "
                     "param check" << std::endl;
    }

    std::cout << ( gFailures ? "au_test: FAILURES\n" : "au_test: all checks passed\n" );
    return gFailures ? 1 : 0;
#endif
}
