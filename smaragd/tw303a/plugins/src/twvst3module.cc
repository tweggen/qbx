// VST3 module loading (proposal 08 M6).
//
// The shape follows twclapmodule.cc deliberately — interning by path, a
// weak_ptr table, the same failure-path deadlock avoidance — because the two
// backends have the same lifetime problem and one solved version of it is worth
// more than two clever ones.
//
// What is genuinely different from CLAP: a .vst3 is a BUNDLE on every platform
// in the 3.6.10+ layout, with a per-architecture subdirectory, and Windows also
// still allows a plain DLL renamed to .vst3. Both are in the wild. This is the
// same flat-vs-bundle split that broke the CLAP loader on macOS in M7, so it is
// handled here from the start rather than discovered on the first user's machine.

#include "twvst3module.h"

#include "tw/core/twlog.h"

#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

#if defined( _WIN32 )
#include <windows.h>
#else
#include <dirent.h>
#include <dlfcn.h>
#include <sys/stat.h>
#endif

#if defined( __APPLE__ )
#include <CoreFoundation/CoreFoundation.h>
#endif

using namespace Steinberg;

namespace audio {

namespace {

// --- module interning ---------------------------------------------------------
std::mutex &moduleMutex()
{
    static std::mutex m;
    return m;
}

std::map<std::string, std::weak_ptr<twVst3Module>> &moduleTable()
{
    static std::map<std::string, std::weak_ptr<twVst3Module>> t;
    return t;
}

#if defined( _WIN32 )
std::wstring widen( const std::string &s )
{
    if( s.empty() ) return std::wstring();
    const int n = MultiByteToWideChar( CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0 );
    if( n <= 0 ) return std::wstring();
    std::wstring w( (std::size_t)n, L'\0' );
    MultiByteToWideChar( CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n );
    return w;
}
#endif

bool isRegularFile( const std::string &p )
{
#if defined( _WIN32 )
    const DWORD a = GetFileAttributesA( p.c_str() );
    return a != INVALID_FILE_ATTRIBUTES && !( a & FILE_ATTRIBUTE_DIRECTORY );
#else
    struct stat st;
    return ::stat( p.c_str(), &st ) == 0 && S_ISREG( st.st_mode );
#endif
}

bool isDir( const std::string &p )
{
#if defined( _WIN32 )
    const DWORD a = GetFileAttributesA( p.c_str() );
    return a != INVALID_FILE_ATTRIBUTES && ( a & FILE_ATTRIBUTE_DIRECTORY );
#else
    struct stat st;
    return ::stat( p.c_str(), &st ) == 0 && S_ISDIR( st.st_mode );
#endif
}

// The lexicographically first regular file in dir — CFBundleExecutable without
// parsing a plist or linking CoreFoundation, and deterministic when a bundle
// holds more than one. Same fallback the CLAP loader needed in M7.
std::string soleFileIn( const std::string &dir )
{
    std::string best;
#if defined( _WIN32 )
    WIN32_FIND_DATAA fd;
    const HANDLE h = FindFirstFileA( ( dir + "\\*" ).c_str(), &fd );
    if( h == INVALID_HANDLE_VALUE ) return best;
    do {
        if( fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) continue;
        const std::string name = fd.cFileName;
        if( best.empty() || name < best ) best = name;
    } while( FindNextFileA( h, &fd ) );
    FindClose( h );
#else
    if( DIR *d = ::opendir( dir.c_str() ) ) {
        while( struct dirent *e = ::readdir( d ) ) {
            const std::string name = e->d_name;
            if( name == "." || name == ".." ) continue;
            if( !isRegularFile( dir + "/" + name ) ) continue;
            if( best.empty() || name < best ) best = name;
        }
        ::closedir( d );
    }
#endif
    return best.empty() ? best : dir + "/" + best;
}

// The architecture folder(s) a bundle may carry on this platform, most specific
// first. arm64ec first on Windows-on-ARM because an emulation-compatible build
// is preferable to the x64 one when both ship.
std::vector<std::string> archDirs()
{
#if defined( _WIN32 )
#if defined( __aarch64__ ) || defined( _M_ARM64 )
    return { "arm64ec-win", "arm64-win", "x86_64-win" };
#else
    return { "x86_64-win" };
#endif
#elif defined( __APPLE__ )
    return { "MacOS" };
#else
#if defined( __aarch64__ )
    return { "aarch64-linux", "arm64-linux" };
#else
    return { "x86_64-linux" };
#endif
#endif
}

std::string leafOf( const std::string &p )
{
    const std::size_t slash = p.find_last_of( "/\\" );
    return slash == std::string::npos ? p : p.substr( slash + 1 );
}

// Resolve the binary to load for a .vst3 path.
//
// Empty return means "nothing loadable here" — the caller logs and gives up
// rather than handing dlopen a directory.
std::string resolveBinary( const std::string &path )
{
    std::string p = path;
    while( p.size() > 1 && ( p.back() == '/' || p.back() == '\\' ) ) p.pop_back();

    // A plain file: Windows' legacy "DLL renamed .vst3". Nothing to resolve.
    if( isRegularFile( p ) ) return p;
    if( !isDir( p ) ) return std::string();

    const std::string base = leafOf( p );          // "Foo.vst3"
    std::string       stem = base;                 // "Foo"
    const std::size_t dot  = stem.find_last_of( '.' );
    if( dot != std::string::npos ) stem = stem.substr( 0, dot );

    for( const std::string &arch : archDirs() ) {
        const std::string dir = p + "/Contents/" + arch;
        if( !isDir( dir ) ) continue;
        // Windows names the inner binary Foo.vst3; macOS/Linux name it Foo (and
        // Linux sometimes Foo.so). Try the conventional spellings, then fall
        // back to whatever single file is in there.
        for( const std::string &cand : { base, stem, stem + ".so" } ) {
            const std::string full = dir + "/" + cand;
            if( isRegularFile( full ) ) return full;
        }
        const std::string sole = soleFileIn( dir );
        if( !sole.empty() ) return sole;
    }
    return std::string();
}

void *symbolOf( void *handle, const char *name )
{
    if( !handle ) return nullptr;
#if defined( _WIN32 )
    return (void *)GetProcAddress( (HMODULE)handle, name );
#else
    return dlsym( handle, name );
#endif
}

}  // namespace

// --- uid <-> TUID -------------------------------------------------------------
//
// A persisted VST3 uid is ALWAYS 32 hex digits in COM (Windows GUID) byte
// order, on every platform this engine runs on. That is not a preference
// stated here for the first time — it is what already sits in every
// committed .qxp and .qxa, because the VST3 SDK's own COM_COMPATIBLE switch
// (pluginterfaces/base/fplatform.h) is 1 only on Windows, and INLINE_UID(l1,
// l2,l3,l4) lays the SAME four 32-bit values out DIFFERENTLY depending on it:
// Windows packs them as a Win32 GUID (Data1/Data2/Data3 little-endian, Data4
// verbatim); Linux/macOS pack them straight (all four big-endian, back to
// back). Before this fix, vst3UidFromTuid() was a byte-for-byte hex dump of
// whichever layout THIS build's TUID happened to be in, so the identical
// INLINE_UID(...) source line in twtestvst3.cpp produced two DIFFERENT
// strings depending on the platform that compiled it — confirmed here: the
// same declaration hex-dumped as "4554575454535356543353494E450000" under
// the (Windows-only) COM_COMPATIBLE=1 layout and as
// "5457455453545653543353494E450000" under the Linux/macOS straight one.
// vst3UidFromTuid() is what fills <SPluginSlot uid=...>, so a project saved
// on Windows silently lost its VST3 plugins on Linux/macOS and vice versa —
// the slot fell back to the transparent placeholder with no error a user
// could act on.
//
// FUID::getLong1..4()/to4Int() (funknown.cpp) already decode a TUID's four
// 32-bit values CORRECTLY regardless of platform — that decode is exactly
// the COM_COMPATIBLE-conditional half this function must NOT hand-roll, per
// the design note above it. What the SDK has no ready-made function for is
// the encode half done UNCONDITIONALLY: "give me the COM/GUID byte order
// always, whichever platform I'm running on" (FUID::from4Int() only ever
// gives the NATIVE order, i.e. exactly COM order on Windows and exactly
// straight order elsewhere — the very asymmetry causing the bug). That one
// packing formula — the COM_COMPATIBLE branch of INLINE_UID/from4Int, lifted
// out from behind its #if so it always runs — is hand-rolled below, and nothing
// else is: on a COM_COMPATIBLE (Windows) build the TUID is ALREADY in that
// order, so decode-then-reencode is a no-op and the output is byte-identical
// to the pre-fix dump — every existing Windows-authored .qxp keeps resolving
// with NO change on that platform. On Linux/macOS it now produces that same
// Windows spelling instead of the platform-local one.
namespace {

// Pack (l1,l2,l3,l4) into COM/GUID byte order (Data1/Data2/Data3
// little-endian, Data4 big-endian) — the COM_COMPATIBLE branch of the SDK's
// own INLINE_UID/FUID::from4Int, reproduced here WITHOUT the #if so it runs
// on every platform, not only the one the SDK would apply it on natively.
void packComOrder( uint32 l1, uint32 l2, uint32 l3, uint32 l4, uint8_t out[16] )
{
    out[0]  = (uint8_t)( ( l1 & 0x000000FF )       );
    out[1]  = (uint8_t)( ( l1 & 0x0000FF00 ) >>  8 );
    out[2]  = (uint8_t)( ( l1 & 0x00FF0000 ) >> 16 );
    out[3]  = (uint8_t)( ( l1 & 0xFF000000 ) >> 24 );
    out[4]  = (uint8_t)( ( l2 & 0x00FF0000 ) >> 16 );
    out[5]  = (uint8_t)( ( l2 & 0xFF000000 ) >> 24 );
    out[6]  = (uint8_t)( ( l2 & 0x000000FF )       );
    out[7]  = (uint8_t)( ( l2 & 0x0000FF00 ) >>  8 );
    out[8]  = (uint8_t)( ( l3 & 0xFF000000 ) >> 24 );
    out[9]  = (uint8_t)( ( l3 & 0x00FF0000 ) >> 16 );
    out[10] = (uint8_t)( ( l3 & 0x0000FF00 ) >>  8 );
    out[11] = (uint8_t)( ( l3 & 0x000000FF )       );
    out[12] = (uint8_t)( ( l4 & 0xFF000000 ) >> 24 );
    out[13] = (uint8_t)( ( l4 & 0x00FF0000 ) >> 16 );
    out[14] = (uint8_t)( ( l4 & 0x0000FF00 ) >>  8 );
    out[15] = (uint8_t)( ( l4 & 0x000000FF )       );
}

// Inverse of packComOrder: recover (l1,l2,l3,l4) from COM/GUID-ordered bytes.
void unpackComOrder( const uint8_t in[16], uint32 &l1, uint32 &l2,
                     uint32 &l3, uint32 &l4 )
{
    l1 = ( (uint32) in[0] )        | ( (uint32) in[1] << 8 )
       | ( (uint32) in[2] << 16 )  | ( (uint32) in[3] << 24 );
    l2 = ( (uint32) in[6] )        | ( (uint32) in[7] << 8 )
       | ( (uint32) in[4] << 16 )  | ( (uint32) in[5] << 24 );
    l3 = ( (uint32) in[11] )       | ( (uint32) in[10] << 8 )
       | ( (uint32) in[9]  << 16 ) | ( (uint32) in[8]  << 24 );
    l4 = ( (uint32) in[15] )       | ( (uint32) in[14] << 8 )
       | ( (uint32) in[13] << 16 ) | ( (uint32) in[12] << 24 );
}

}  // namespace

std::string vst3UidFromTuid( const TUID uid )
{
    uint32 l1, l2, l3, l4;
    FUID( FUID::fromTUID( uid ) ).to4Int( l1, l2, l3, l4 );   // platform-aware decode

    uint8_t com[16];
    packComOrder( l1, l2, l3, l4, com );                      // unconditional COM encode

    char buf[33];
    for( int i = 0; i < 16; ++i )
        std::snprintf( buf + i * 2, 3, "%02X", com[i] );
    return std::string( buf, 32 );
}

bool vst3TuidFromUid( const std::string &uid, TUID out )
{
    if( uid.size() != 32 ) return false;
    uint8_t com[16];
    for( int i = 0; i < 16; ++i ) {
        int v = 0;
        for( int k = 0; k < 2; ++k ) {
            const char c = uid[(std::size_t)( i * 2 + k )];
            int        d;
            if( c >= '0' && c <= '9' )      d = c - '0';
            else if( c >= 'A' && c <= 'F' ) d = c - 'A' + 10;
            else if( c >= 'a' && c <= 'f' ) d = c - 'a' + 10;
            else return false;
            v = ( v << 4 ) | d;
        }
        com[i] = (uint8_t) v;
    }

    uint32 l1, l2, l3, l4;
    unpackComOrder( com, l1, l2, l3, l4 );   // undo the unconditional COM encode
    FUID f( l1, l2, l3, l4 );                // platform-aware native construction
    f.toTUID( out );
    return true;
}

// --- twVst3Module -------------------------------------------------------------

twVst3Module::~twVst3Module()
{
    {
        std::lock_guard<std::mutex> lock( moduleMutex() );
        auto it = moduleTable().find( path_ );
        // Only drop the entry if it still refers to us: a concurrent open() that
        // found an expired weak_ptr may already have installed a replacement.
        if( it != moduleTable().end() && it->second.expired() )
            moduleTable().erase( it );
    }
    unload();
}

bool twVst3Module::load( const std::string &path )
{
    path_ = path;

    const std::string binary = resolveBinary( path );
    if( binary.empty() ) {
        TW_LOGE( "plugins", "[vst3] no loadable binary in '%s' "
                 "(neither a plain module nor a Contents/<arch>/ bundle)", path.c_str() );
        return false;
    }

    const char *initName = nullptr;

#if defined( _WIN32 )
    const std::wstring wide = widen( binary );
    if( wide.empty() ) {
        TW_LOGE( "plugins", "[vst3] cannot widen module path '%s'", binary.c_str() );
        return false;
    }
    // LOAD_WITH_ALTERED_SEARCH_PATH: the module's own directory becomes the first
    // stop for its dependent DLLs, which is how plugin vendors ship helper libs.
    //
    // SEM_FAILCRITICALERRORS for the duration of the load, per-THREAD so a
    // concurrent scan cannot disturb the host app's error-mode state: a
    // truncated or wrong-architecture DLL otherwise raises a MODAL "Bad Image"
    // box, which no scan may ever do.
    DWORD      prevErrMode = 0;
    const BOOL errModeSet  = SetThreadErrorMode(
        SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &prevErrMode );

    HMODULE h = LoadLibraryExW( wide.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH );

    if( errModeSet ) SetThreadErrorMode( prevErrMode, nullptr );

    if( !h ) {
        TW_LOGE( "plugins", "[vst3] LoadLibraryExW failed for '%s' (error %lu)",
                 binary.c_str(), (unsigned long)GetLastError() );
        return false;
    }
    handle_   = (void *)h;
    initName  = "InitDll";
    exitName_ = "ExitDll";
#else
    void *h = dlopen( binary.c_str(), RTLD_LOCAL | RTLD_NOW );
    if( !h ) {
        const char *why = dlerror();
        TW_LOGE( "plugins", "[vst3] dlopen failed for '%s': %s", binary.c_str(),
                 why ? why : "(unknown)" );
        return false;
    }
    handle_ = h;
#if defined( __APPLE__ )
    initName  = "bundleEntry";
    exitName_ = "bundleExit";
#else
    initName  = "ModuleEntry";
    exitName_ = "ModuleExit";
#endif
#endif

#if defined( __APPLE__ )
    // bundleEntry's argument is a CFBundleRef and it is NOT optional in
    // practice, however much the spec's wording suggests a host may skip it.
    // This used to pass null unconditionally, with a comment claiming plugins
    // "use it only for resource lookup" — that is wrong for every plugin built
    // on the stock VST3 SDK or on iPlug2, both of which CFRetain the ref the
    // moment they are handed it. Measured: passing null SEGFAULTS inside the
    // plugin, three frames deep and with no diagnostic a user could act on —
    //
    //   frame #0: CoreFoundation`CFRetain + 24     EXC_BAD_ACCESS
    //   frame #1: NassauEQ`bundleEntry + 52
    //   frame #2: twVst3Module::load()
    //
    // — so on macOS a bundle is now opened AS a bundle and named honestly.
    // The executable is still brought in by dlopen above (that half always
    // worked, and it keeps symbol resolution identical on every platform);
    // CFBundleCreate here is purely so bundleEntry has something real to
    // retain and to look its resources up in. The ref is held for the module's
    // lifetime and released in unload(), because the plugin keeps a retain on
    // it until bundleExit.
    //
    // A FLAT module — a dylib renamed .vst3, which is exactly what the in-repo
    // twtestvst3 fixture is — has no bundle, so cfBundle_ stays null and the
    // call below is byte-for-byte the old one. That is deliberate: it is also
    // the only shape any headless gate on this platform can reach.
    if( isDir( path ) ) {
        if( CFStringRef sp = CFStringCreateWithCString( kCFAllocatorDefault,
                                                        path.c_str(),
                                                        kCFStringEncodingUTF8 ) ) {
            if( CFURLRef url = CFURLCreateWithFileSystemPath(
                    kCFAllocatorDefault, sp, kCFURLPOSIXPathStyle, true ) ) {
                cfBundle_ = (void *)CFBundleCreate( kCFAllocatorDefault, url );
                CFRelease( url );
            }
            CFRelease( sp );
        }
        if( !cfBundle_ )
            TW_LOGW( "plugins", "[vst3] '%s' is a bundle but CFBundleCreate failed; "
                     "calling bundleEntry with no bundle reference",
                     path.c_str() );
    }
#endif

    // The entry point is optional in the spec and universal in practice. A
    // plugin that has one and does not get it called usually crashes later
    // rather than here, so a miss is worth a line in the log.
    if( void *entry = symbolOf( handle_, initName ) ) {
#if defined( _WIN32 )
        inited_ = ( (bool ( * )())entry )();
#elif defined( __APPLE__ )
        // bundleEntry( CFBundleRef ) — the real ref for a bundle, null for a
        // flat module, per the note above.
        inited_ = ( (bool ( * )( void * ))entry )( cfBundle_ );
#else
        // ModuleEntry( void* ) on Linux; the argument is unused there.
        inited_ = ( (bool ( * )( void * ))entry )( nullptr );
#endif
        if( !inited_ ) {
            TW_LOGE( "plugins", "[vst3] %s() returned false for '%s'", initName,
                     binary.c_str() );
            return false;
        }
    } else {
        TW_LOGD( "plugins", "[vst3] '%s' exports no %s (allowed)", binary.c_str(), initName );
    }

    void *getFactory = symbolOf( handle_, "GetPluginFactory" );
    if( !getFactory ) {
        TW_LOGE( "plugins", "[vst3] '%s' exports no GetPluginFactory symbol",
                 binary.c_str() );
        return false;
    }

    factory_ = ( (IPluginFactory * ( * )())getFactory )();
    if( !factory_ ) {
        TW_LOGE( "plugins", "[vst3] GetPluginFactory() returned null for '%s'",
                 binary.c_str() );
        return false;
    }

    TW_LOGI( "plugins", "[vst3] loaded '%s' (%d class(es))", path.c_str(),
             (int)factory_->countClasses() );
    return true;
}

void twVst3Module::unload()
{
    if( factory_ ) {
        factory_->release();
        factory_ = nullptr;
    }
    if( handle_ ) {
        if( inited_ && exitName_ ) {
            if( void *e = symbolOf( handle_, exitName_ ) )
                ( (bool ( * )())e )();
        }
#if defined( _WIN32 )
        FreeLibrary( (HMODULE)handle_ );
#else
        dlclose( handle_ );
#endif
        handle_ = nullptr;
    }
#if defined( __APPLE__ )
    // Released only AFTER bundleExit above: the plugin holds its own retain
    // until then, so ours is what keeps the ref alive across the exit call.
    if( cfBundle_ ) {
        CFRelease( (CFBundleRef)cfBundle_ );
        cfBundle_ = nullptr;
    }
#endif
    inited_   = false;
    exitName_ = nullptr;
}

std::shared_ptr<twVst3Module> twVst3Module::open( const std::string &path )
{
    if( path.empty() ) return nullptr;

    // make_shared is not usable: the constructor is private.
    std::shared_ptr<twVst3Module> mod;
    bool                          loaded = false;

    {
        std::lock_guard<std::mutex> lock( moduleMutex() );

        auto it = moduleTable().find( path );
        if( it != moduleTable().end() ) {
            if( std::shared_ptr<twVst3Module> live = it->second.lock() )
                return live;   // a COPY leaves the scope; nothing is destroyed
            moduleTable().erase( it );
        }

        mod.reset( new twVst3Module() );
        loaded = mod->load( path );
        if( loaded ) moduleTable()[path] = mod;
    }

    // A FAILED load must release `mod` with the table mutex RELEASED:
    // ~twVst3Module takes that same NON-RECURSIVE mutex to un-intern itself, so
    // destroying the half-built module inside the critical section deadlocks the
    // caller against itself. The CLAP loader learned this the hard way when the
    // M2 scanner met its first corrupt module; inheriting the fix is free.
    if( !loaded ) mod.reset();
    return mod;
}

// --- descriptors --------------------------------------------------------------

std::vector<twPluginDescriptor> vst3ModuleDescriptors( const std::string &path )
{
    std::vector<twPluginDescriptor> out;

    std::shared_ptr<twVst3Module> mod = twVst3Module::open( path );
    if( !mod || !mod->factory() ) return out;

    IPluginFactory *factory = mod->factory();

    // Vendor comes from the factory when a class does not carry its own.
    std::string factoryVendor;
    PFactoryInfo fi{};
    if( factory->getFactoryInfo( &fi ) == kResultOk ) {
        std::size_t n = 0;
        while( n < (std::size_t)PFactoryInfo::kNameSize && fi.vendor[n] ) ++n;
        factoryVendor.assign( fi.vendor, n );
    }

    IPluginFactory2 *f2 = nullptr;
    factory->queryInterface( IPluginFactory2::iid, (void **)&f2 );

    const int32 nClasses = factory->countClasses();
    for( int32 i = 0; i < nClasses; ++i ) {
        PClassInfo ci{};
        if( factory->getClassInfo( i, &ci ) != kResultOk ) continue;
        if( std::strncmp( ci.category, kVstAudioEffectClass,
                          PClassInfo::kCategorySize ) != 0 )
            continue;   // controllers, ARA factories and the rest are not ours

        twPluginDescriptor d;
        d.format = "vst3";
        d.uid    = vst3UidFromTuid( ci.cid );
        d.path   = path;

        std::size_t n = 0;
        while( n < (std::size_t)PClassInfo::kNameSize && ci.name[n] ) ++n;
        d.name.assign( ci.name, n );
        d.vendor = factoryVendor;

        if( f2 ) {
            PClassInfo2 ci2{};
            if( f2->getClassInfo2( i, &ci2 ) == kResultOk ) {
                std::size_t v = 0;
                while( v < (std::size_t)PClassInfo2::kVendorSize && ci2.vendor[v] ) ++v;
                if( v ) d.vendor.assign( ci2.vendor, v );

                std::size_t s = 0;
                while( s < (std::size_t)PClassInfo2::kSubCategoriesSize && ci2.subCategories[s] )
                    ++s;
                // VST3 has no separate instrument CATEGORY: an instrument is an
                // audio-effect class whose subCategories say "Instrument".
                d.isInstrument =
                    std::string( ci2.subCategories, s ).find( "Instrument" ) != std::string::npos;
            }
        }

        // The declared I/O is not in the class metadata — it takes a live
        // instance to ask. Creating one is the only honest answer, and it is
        // also what makes an out-of-process probe worth having (M2).
        d.io = twPluginIoLayout{ 0, 0 };
        if( std::unique_ptr<twPlugin> inst = createVst3Plugin( path, d.uid ) ) {
            d.io = inst->ioLayout();

            // Scanner version 2 (proposal 37 P2): the event buses and the aux
            // audio outs, read off the SAME instance the I/O came from.
            const twPluginCapabilities caps = inst->capabilities();
            d.acceptsNotes  = caps.acceptsNotes;
            d.emitsNotes    = caps.emitsNotes;
            d.eventPortsIn  = caps.notePortsIn;
            d.eventPortsOut = caps.notePortsOut;
            d.isInstrument  = d.isInstrument || caps.isInstrument;

            d.nOutBuses = (std::uint16_t)inst->audioOutBusCount();
            d.outBusChannels.clear();
            for( std::size_t b = 0; b < (std::size_t)d.nOutBuses; ++b )
                d.outBusChannels.push_back( inst->audioOutBus( b ).channels );
        }

        out.push_back( std::move( d ) );
    }

    if( f2 ) f2->release();

    if( out.empty() )
        TW_LOGW( "plugins", "[vst3] '%s' offers no audio-effect class", path.c_str() );
    return out;
}

}  // namespace audio
