// AudioUnit backend: OS-registry discovery + descriptor enumeration
// (proposal 08 M8). Compiled only on macOS with TW_HAVE_AU (see the AU block in
// tw303a/CMakeLists.txt), so there is no TW_HAVE_AU #ifdef in here.
//
// Everything is the plain C AudioComponent / AudioToolbox / CoreFoundation API —
// no Objective-C — so these are .cc, not .mm. Discovery is AudioComponentFindNext
// over the OS registry (the authoritative list; it covers third-party bundles in
// the Components folders AND Apple's own units, without us walking directories),
// and a component's identity is its (type, subtype, manufacturer) triple.

#include "twaumodule.h"

#include "tw/core/twlog.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <cstdio>
#include <set>
#include <string>

namespace audio {

namespace {

std::string cfToStd( CFStringRef s )
{
    if( !s )
        return std::string();
    if( const char *fast = CFStringGetCStringPtr( s, kCFStringEncodingUTF8 ) )
        return std::string( fast );
    const CFIndex len = CFStringGetLength( s );
    const CFIndex cap = CFStringGetMaximumSizeForEncoding( len, kCFStringEncodingUTF8 ) + 1;
    std::string out( (std::size_t) std::max<CFIndex>( cap, 1 ), '\0' );
    if( !CFStringGetCString( s, &out[0], (CFIndex) out.size(), kCFStringEncodingUTF8 ) )
        return std::string();
    out.resize( std::char_traits<char>::length( out.c_str() ) );
    return out;
}

// The types we host. Effects and music-effects are track inserts (proposal 08);
// MUSIC DEVICES (aumu) and MIDI PROCESSORS (aumi) joined them in proposal 36 P2,
// because the note model they were gated on now exists — a MusicDevice has no
// audio input and is an instrument, which is exactly the shape the instrument
// slot wants.
const std::uint32_t kHostedTypes[] = {
    (std::uint32_t) kAudioUnitType_Effect,
    (std::uint32_t) kAudioUnitType_MusicEffect,
    (std::uint32_t) kAudioUnitType_MusicDevice,
    (std::uint32_t) kAudioUnitType_MIDIProcessor,
};

}  // namespace

// --- uid <-> component codes -------------------------------------------------
// A stable, reversible spelling of the (type, subtype, manufacturer) triple.
// Hex rather than the raw FourCC chars because a subtype/manufacturer may hold
// non-printable bytes; hex round-trips through a project file and a shell arg
// without escaping surprises.

std::string auUidFromCodes( std::uint32_t type, std::uint32_t subtype,
                            std::uint32_t manufacturer )
{
    char buf[32];
    std::snprintf( buf, sizeof( buf ), "%08x-%08x-%08x",
                   (unsigned) type, (unsigned) subtype, (unsigned) manufacturer );
    return std::string( buf );
}

bool auCodesFromUid( const std::string &uid, std::uint32_t &type,
                     std::uint32_t &subtype, std::uint32_t &manufacturer )
{
    const char *s = uid.c_str();
    if( uid.size() > 3 && uid.compare( 0, 3, "au:" ) == 0 )
        s += 3;   // tolerate the "au:<triple>" module-key spelling
    unsigned t = 0, sub = 0, mfr = 0;
    if( std::sscanf( s, "%8x-%8x-%8x", &t, &sub, &mfr ) != 3 )
        return false;
    type         = t;
    subtype      = sub;
    manufacturer = mfr;
    return true;
}

// --- discovery ---------------------------------------------------------------

std::vector<twPluginModuleFile> enumerateAuModules()
{
    std::vector<twPluginModuleFile> out;
    std::set<std::string>           seen;

    for( std::uint32_t type : kHostedTypes ) {
        AudioComponentDescription search;
        search.componentType         = type;
        search.componentSubType      = 0;   // wildcards
        search.componentManufacturer = 0;
        search.componentFlags        = 0;
        search.componentFlagsMask    = 0;

        AudioComponent comp = nullptr;
        while( ( comp = AudioComponentFindNext( comp, &search ) ) != nullptr ) {
            AudioComponentDescription d;
            if( AudioComponentGetDescription( comp, &d ) != noErr )
                continue;

            const std::string uid = auUidFromCodes(
                (std::uint32_t) d.componentType, (std::uint32_t) d.componentSubType,
                (std::uint32_t) d.componentManufacturer );
            const std::string key = "au:" + uid;
            if( !seen.insert( key ).second )
                continue;

            twPluginModuleFile m;
            m.path   = key;
            m.format = "au";
            // Fold the component's own version into the cache key's size field:
            // a plugin that bumps its version re-probes, without us needing a
            // bundle path to stat. mtime stays 0 (there is no file here).
            UInt32 ver = 0;
            AudioComponentGetVersion( comp, &ver );
            m.sizeBytes = (std::uint64_t) ver;
            m.mtimeMs   = 0;
            out.push_back( std::move( m ) );
        }
    }

    std::sort( out.begin(), out.end(),
               []( const twPluginModuleFile &a, const twPluginModuleFile &b ) {
                   return a.path < b.path;
               } );
    TW_LOGI( "plugins", "[au] registry enumeration found %u hostable component(s)",
             (unsigned) out.size() );
    return out;
}

// --- descriptors -------------------------------------------------------------

std::vector<twPluginDescriptor> auModuleDescriptors( const std::string &moduleKey )
{
    std::vector<twPluginDescriptor> out;

    std::uint32_t t = 0, s = 0, mfr = 0;
    if( !auCodesFromUid( moduleKey, t, s, mfr ) ) {
        TW_LOGW( "plugins", "[au] '%s' is not an AU module key", moduleKey.c_str() );
        return out;
    }

    AudioComponentDescription want;
    want.componentType         = t;
    want.componentSubType      = s;
    want.componentManufacturer = mfr;
    want.componentFlags        = 0;
    want.componentFlagsMask    = 0;

    AudioComponent comp = AudioComponentFindNext( nullptr, &want );
    if( !comp ) {
        TW_LOGW( "plugins", "[au] no registered component for '%s'", moduleKey.c_str() );
        return out;
    }

    twPluginDescriptor d;
    d.format       = "au";
    d.uid          = auUidFromCodes( t, s, mfr );
    d.path         = std::string();   // AU identity is the uid; path is cosmetic
    // A MusicDevice IS the instrument type (proposal 36 P2); the capabilities
    // read off the live instance below confirm it.
    d.isInstrument = t == (std::uint32_t) kAudioUnitType_MusicDevice;

    CFStringRef cfName = nullptr;
    if( AudioComponentCopyName( comp, &cfName ) == noErr && cfName ) {
        const std::string full = cfToStd( cfName );
        CFRelease( cfName );
        // AudioComponentCopyName yields "Manufacturer: Name".
        const std::size_t sep = full.find( ": " );
        if( sep != std::string::npos ) {
            d.vendor = full.substr( 0, sep );
            d.name   = full.substr( sep + 2 );
        } else {
            d.name = full;
        }
    }
    if( d.name.empty() )
        d.name = d.uid;

    // Channel counts need a live instance (like CLAP). This instantiation is the
    // one the out-of-process probe isolates: a component that crashes on
    // creation kills the probe and becomes a cached failure, not a dead app.
    d.io = twPluginIoLayout{ 0, 0 };
    if( std::unique_ptr<twPlugin> inst = createAuPlugin( d.path, d.uid ) ) {
        d.io = inst->ioLayout();

        // Scanner version 2 (proposal 36 P2), off the same instance.
        const twPluginCapabilities caps = inst->capabilities();
        d.acceptsNotes  = caps.acceptsNotes;
        d.emitsNotes    = caps.emitsNotes;
        d.eventPortsIn  = caps.notePortsIn;
        d.eventPortsOut = caps.notePortsOut;
        d.isInstrument  = d.isInstrument || caps.isInstrument;

        d.nOutBuses = (std::uint16_t) inst->audioOutBusCount();
        d.outBusChannels.clear();
        for( std::size_t b = 0; b < (std::size_t) d.nOutBuses; ++b )
            d.outBusChannels.push_back( inst->audioOutBus( b ).channels );
    }

    out.push_back( std::move( d ) );
    return out;
}

}  // namespace audio
