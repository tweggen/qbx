// vst3_probe — de-risking spike for proposal 08 M6 (VST3 backend).
//
// This is the GATE the execution plan names: "MinGW ABI note: VST3's interfaces
// are single-inheritance chains from FUnknown, and x64 Windows has one calling
// convention — MSVC-built plugins are loadable from a MinGW host (Ardour does
// exactly this). A spike is still the gate before writing the wrapper."
//
// What it has to answer, before one line of twvst3plugin.cc is written:
//
//   1. Can a binary built by Qt's bundled MinGW g++ dlopen/LoadLibrary a
//      third-party .vst3 that was built by MSVC, and resolve GetPluginFactory?
//   2. Does calling THROUGH the plugin's vtables land on the right methods?
//      (vtable layout + __stdcall-on-x64 + name mangling are all in play here.)
//   3. Does the plugin calling back INTO OUR vtables work? A host-side
//      IBStream, IHostApplication and IComponentHandler are all invoked by the
//      plugin during initialize()/getState(), so a one-directional test would
//      prove only half of the ABI.
//   4. Do the PODs passed across the boundary (PClassInfo, BusInfo,
//      ParameterInfo, ProcessSetup, ProcessData, AudioBusBuffers) agree on
//      layout? falignpush.h uses #pragma pack, so this is a real question and
//      not a formality.
//   5. Does a real process() call produce plausible audio? That is the only
//      check that exercises ProcessData end to end.
//
// It is a SPIKE: not shipped, not a test gate, deliberately one self-contained
// translation unit. It lives under plugins/tools/ because that is the
// established home for engine dev tools (cf. analysis/tools/warp_ab.cc,
// plugins/tools/clap_probe.cc) and because tools/ is an ALLOW_DIR in
// tools/check_logging.py — printing a table to stdout IS this program's output,
// not a diagnostic.
//
//   vst3_probe <path-to-plugin.vst3> [more.vst3 ...]
//
// Exit code 0 if every argument loaded and yielded at least one audio-effect
// class that survived instantiate → initialize → process → teardown; 1
// otherwise; 2 on a usage error.
//
// M6 promotes three things out of here, and nothing else:
//   - the module loader        → src/twvst3module.{h,cc}
//   - the host-side objects    → src/twvst3host.{h,cc}
//   - the DEF_CLASS_IID block  → src/twvst3iids.cc   (see the note on it below)
// The probe itself stays as a spike, exactly as clap_probe.cc did for M0/M1.

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstpluginterfacesupport.h"
#include "pluginterfaces/vst/vstspeaker.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined( _WIN32 )
#include <windows.h>
#else
#include <dirent.h>
#include <dlfcn.h>
#include <sys/stat.h>
#endif

// --- interface IIDs -----------------------------------------------------------
//
// The VST module's IIDs used to be defined right here, inline. That was the
// spike's single most useful FINDING: vst3_pluginterfaces ships base/coreiids.cpp
// which defines the BASE IIDs only (FUnknown, IBStream, IPluginBase,
// IPluginFactory{,2,3}, ...), while IComponent, IAudioProcessor, IEditController
// and every host interface live in public.sdk/source/vst/vstinitiids.cpp in the
// FULL SDK — which this submodule does not contain. Without our own definitions
// the four SDK sources named by 08_PLUGIN_HOSTING_EXECUTION.md M6 build, link,
// and then fail on the first `IComponent::iid` reference.
//
// M6 promoted the block to plugins/src/twvst3iids.cc, which tw_plugins links.
// The spike links tw_plugins, so it gets them from there — defining them again
// here would be a duplicate-symbol error.

using namespace Steinberg;

namespace {

int gWarnings = 0;

// --- run-time flags (declared here because ComponentHandler reads gTrace) ----
bool gWantView    = false;  // --view : walk the native editor
bool gShow        = false;  // --show : make the window VISIBLE and pump for real
bool gTrace       = false;  // print every component-handler callback as it lands
int  gShowSeconds = 30;     // how long --show keeps the window up

void warn( const char *fmt, ... )
{
    std::printf( "    ! " );
    va_list ap;
    va_start( ap, fmt );
    std::vprintf( fmt, ap );
    va_end( ap );
    std::printf( "\n" );
    ++gWarnings;
}

// --- small helpers ------------------------------------------------------------

// String128 is char16[128] (UTF-16). Enough of a transcode to print a plugin
// name; astral characters are not worth the code in a probe.
std::string u16ToUtf8( const Vst::TChar *s, std::size_t maxLen = 128 )
{
    std::string out;
    if( !s ) return out;
    for( std::size_t i = 0; i < maxLen && s[i]; ++i ) {
        const std::uint32_t c = (std::uint32_t)(std::uint16_t)s[i];
        if( c < 0x80 ) {
            out.push_back( (char)c );
        } else if( c < 0x800 ) {
            out.push_back( (char)( 0xC0 | ( c >> 6 ) ) );
            out.push_back( (char)( 0x80 | ( c & 0x3F ) ) );
        } else {
            out.push_back( (char)( 0xE0 | ( c >> 12 ) ) );
            out.push_back( (char)( 0x80 | ( ( c >> 6 ) & 0x3F ) ) );
            out.push_back( (char)( 0x80 | ( c & 0x3F ) ) );
        }
    }
    return out;
}

// The SDK's metadata fields are fixed char8 arrays that a plugin is supposed to
// null-terminate. Bound the read rather than trusting it: a probe exists to
// survive malformed modules.
std::string fixedStr( const char8 *p, std::size_t cap )
{
    if( !p ) return std::string();
    std::size_t n = 0;
    while( n < cap && p[n] ) ++n;
    return std::string( p, n );
}

std::string tuidToHex( const TUID uid )
{
    char buf[33];
    for( int i = 0; i < 16; ++i )
        std::snprintf( buf + i * 2, 3, "%02X", (unsigned char)uid[i] );
    return std::string( buf, 32 );
}

const char *resultName( tresult r )
{
    if( r == kResultOk )          return "kResultOk";
    if( r == kResultFalse )       return "kResultFalse";
    if( r == kNoInterface )       return "kNoInterface";
    if( r == kNotImplemented )    return "kNotImplemented";
    if( r == kInvalidArgument )   return "kInvalidArgument";
    return "<other>";
}

bool fileExists( const std::string &p )
{
#if defined( _WIN32 )
    const DWORD a = GetFileAttributesA( p.c_str() );
    return a != INVALID_FILE_ATTRIBUTES && !( a & FILE_ATTRIBUTE_DIRECTORY );
#else
    struct stat st;
    return ::stat( p.c_str(), &st ) == 0 && S_ISREG( st.st_mode );
#endif
}

bool isDirectory( const std::string &p )
{
#if defined( _WIN32 )
    const DWORD a = GetFileAttributesA( p.c_str() );
    return a != INVALID_FILE_ATTRIBUTES && ( a & FILE_ATTRIBUTE_DIRECTORY );
#else
    struct stat st;
    return ::stat( p.c_str(), &st ) == 0 && S_ISDIR( st.st_mode );
#endif
}

std::string baseName( const std::string &p )
{
    const std::size_t slash = p.find_last_of( "/\\" );
    return slash == std::string::npos ? p : p.substr( slash + 1 );
}

// --- host-side objects --------------------------------------------------------
//
// Hand-rolled rather than derived from funknownimpl.h's helpers, on purpose:
// the point of the spike is to see OUR OWN vtables being called across the
// module boundary. A single non-atomic refcount is fine — everything here runs
// on one thread.
//
// M6 promotes these to src/twvst3host.{h,cc}, where the refcount becomes
// std::atomic and IHostApplication::createInstance grows real IMessage /
// IAttributeList implementations (a plugin that needs them to talk between its
// component and controller gets kNotImplemented here, which is enough to load
// but not enough to ship).

template <class Iface>
class HostObject : public Iface {
public:
    uint32 PLUGIN_API addRef() override { return ++refs_; }
    uint32 PLUGIN_API release() override
    {
        // Deliberately NOT self-deleting: every host object in this probe is a
        // stack local outliving the plugin that borrows it. A plugin that
        // over-releases would otherwise corrupt the stack instead of showing up
        // as a refcount the report can print.
        if( refs_ > 0 ) --refs_;
        return refs_;
    }
    uint32 refs() const { return refs_; }

protected:
    // Answers the FUnknown leg for everyone; each subclass adds its own.
    tresult resolve( const TUID _iid, void **obj, const FUID &mine )
    {
        if( FUnknownPrivate::iidEqual( _iid, FUnknown::iid ) ||
            FUnknownPrivate::iidEqual( _iid, mine ) ) {
            *obj = this;
            this->addRef();
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }

private:
    uint32 refs_ = 1;
};

// A memory IBStream. getState()/setState() drive it from INSIDE the plugin, so
// it is the cheapest proof that plugin → host virtual calls land correctly and
// that our `int32*`/`int64*` out-params are read back the way we wrote them.
class MemStream : public HostObject<IBStream> {
public:
    tresult PLUGIN_API queryInterface( const TUID _iid, void **obj ) override
    {
        return resolve( _iid, obj, IBStream::iid );
    }

    tresult PLUGIN_API read( void *buffer, int32 numBytes, int32 *numRead ) override
    {
        if( !buffer || numBytes < 0 ) return kInvalidArgument;
        const int64 avail = (int64)data_.size() - pos_;
        const int32 n = (int32)std::min<int64>( numBytes, std::max<int64>( 0, avail ) );
        if( n > 0 ) std::memcpy( buffer, data_.data() + pos_, (std::size_t)n );
        pos_ += n;
        if( numRead ) *numRead = n;
        return kResultOk;
    }

    tresult PLUGIN_API write( void *buffer, int32 numBytes, int32 *numWritten ) override
    {
        if( !buffer || numBytes < 0 ) return kInvalidArgument;
        if( pos_ + numBytes > (int64)data_.size() )
            data_.resize( (std::size_t)( pos_ + numBytes ) );
        std::memcpy( data_.data() + pos_, buffer, (std::size_t)numBytes );
        pos_ += numBytes;
        if( numWritten ) *numWritten = numBytes;
        return kResultOk;
    }

    tresult PLUGIN_API seek( int64 pos, int32 mode, int64 *result ) override
    {
        int64 base = 0;
        if( mode == kIBSeekSet )      base = 0;
        else if( mode == kIBSeekCur ) base = pos_;
        else if( mode == kIBSeekEnd ) base = (int64)data_.size();
        else return kInvalidArgument;

        const int64 want = base + pos;
        if( want < 0 ) return kInvalidArgument;
        pos_ = want;
        if( result ) *result = pos_;
        return kResultOk;
    }

    tresult PLUGIN_API tell( int64 *pos ) override
    {
        if( !pos ) return kInvalidArgument;
        *pos = pos_;
        return kResultOk;
    }

    void   rewind()      { pos_ = 0; }
    std::size_t size() const { return data_.size(); }

private:
    std::vector<std::uint8_t> data_;
    int64                     pos_ = 0;
};

// Reports which optional host interfaces we support. Several plugins query this
// during initialize() and take a different (usually simpler) path when it says
// no — so answering honestly is part of loading correctly, not politeness.
class PlugInterfaceSupport : public HostObject<Vst::IPlugInterfaceSupport> {
public:
    tresult PLUGIN_API queryInterface( const TUID _iid, void **obj ) override
    {
        return resolve( _iid, obj, Vst::IPlugInterfaceSupport::iid );
    }

    tresult PLUGIN_API isPlugInterfaceSupported( const TUID _iid ) override
    {
        // The spike supports nothing beyond the mandatory set. Saying kResultOk
        // to something we have not implemented is how a host earns a crash.
        (void)_iid;
        return kResultFalse;
    }
};

// --- IPlugFrame: the reverse leg of the GUI ABI (proposal 33 PoC) -------------
//
// The whole reason the view step exists. createView()/attached() prove that WE
// can call INTO the plugin's view vtable; resizeView() is the only thing that
// proves the plugin can call back OUT into a GUI vtable of ours, and that is the
// half a one-directional test would miss — exactly the argument question 3 at
// the top of this file makes for IBStream and IComponentHandler.
//
// On Linux a real host must also answer IRunLoop here or an X11 plugin never
// repaints. This spike is Windows-first and says so rather than pretending.
class PlugFrame : public HostObject<IPlugFrame> {
public:
    tresult PLUGIN_API queryInterface( const TUID _iid, void **obj ) override
    {
        return resolve( _iid, obj, IPlugFrame::iid );
    }

    tresult PLUGIN_API resizeView( IPlugView *view, ViewRect *rect ) override
    {
        if( !rect ) return kInvalidArgument;
        ++resizes;
        lastW = rect->getWidth();
        lastH = rect->getHeight();
        std::printf( "    view   : plugin requested resize -> %dx%d (IPlugFrame CALLED BACK)\n",
                     lastW, lastH );
        // A real host resizes its container and then tells the view it happened.
        // Accepting the plugin's own number unchanged is the honest spike
        // behaviour: it is what a host that always grants the request would do.
        if( view ) view->onSize( rect );
        return kResultOk;
    }

    int resizes = 0;
    int lastW   = 0;
    int lastH   = 0;
};

class HostApp : public HostObject<Vst::IHostApplication> {
public:
    explicit HostApp( PlugInterfaceSupport *support ) : support_( support ) {}

    tresult PLUGIN_API queryInterface( const TUID _iid, void **obj ) override
    {
        if( FUnknownPrivate::iidEqual( _iid, Vst::IPlugInterfaceSupport::iid ) ) {
            *obj = support_;
            support_->addRef();
            return kResultOk;
        }
        return resolve( _iid, obj, Vst::IHostApplication::iid );
    }

    tresult PLUGIN_API getName( Vst::String128 name ) override
    {
        static const char16_t kName[] = u"Smaragd (vst3_probe)";
        const std::size_t n = sizeof( kName ) / sizeof( kName[0] );
        for( std::size_t i = 0; i < n && i < 128; ++i )
            name[i] = (Vst::TChar)kName[i];
        return kResultOk;
    }

    tresult PLUGIN_API createInstance( TUID cid, TUID _iid, void **obj ) override
    {
        // IMessage / IAttributeList would go here (M6). Recording the request is
        // the useful part for a spike: it tells us whether the plugins we care
        // about actually need them before we write them.
        std::printf( "    host: createInstance requested cid=%s iid=%s -> kNotImplemented\n",
                     tuidToHex( cid ).c_str(), tuidToHex( _iid ).c_str() );
        ++createInstanceRequests;
        if( obj ) *obj = nullptr;
        return kNotImplemented;
    }

    int createInstanceRequests = 0;

private:
    PlugInterfaceSupport *support_;
};

class ComponentHandler : public HostObject<Vst::IComponentHandler> {
public:
    tresult PLUGIN_API queryInterface( const TUID _iid, void **obj ) override
    {
        return resolve( _iid, obj, Vst::IComponentHandler::iid );
    }

    // The three gestures are counted SEPARATELY and their payloads printed,
    // because proposal 33 §2 turns on exactly this: the production backend's
    // performEdit (twvst3host.cc:304-311) takes ParamID and ParamValue and
    // DISCARDS them both, so the host cannot know which parameter moved or to
    // what. This handler keeps them, which is the only way to find out whether
    // a real plugin's GUI actually delivers them — and whether beginEdit /
    // endEdit really bracket a drag, which is what makes one gesture one undo
    // entry and one automation punch-in.
    tresult PLUGIN_API beginEdit( Vst::ParamID id ) override
    {
        ++edits;
        ++begins;
        if( gTrace ) std::printf( "    edit   : beginEdit   id=%u\n", (unsigned)id );
        return kResultOk;
    }

    tresult PLUGIN_API performEdit( Vst::ParamID id, Vst::ParamValue v ) override
    {
        ++edits;
        ++performs;
        lastId    = id;
        lastValue = v;
        if( gTrace ) std::printf( "    edit   : performEdit id=%u value=%.6f\n", (unsigned)id, v );
        return kResultOk;
    }

    tresult PLUGIN_API endEdit( Vst::ParamID id ) override
    {
        ++edits;
        ++ends;
        if( gTrace ) std::printf( "    edit   : endEdit     id=%u\n", (unsigned)id );
        return kResultOk;
    }

    tresult PLUGIN_API restartComponent( int32 flags ) override
    {
        lastRestartFlags = flags;
        ++restarts;
        // The production backend discards these flags entirely
        // (twvst3host.cc:322, `(void)flags;`), so kLatencyChanged,
        // kParamValuesChanged and kReloadComponent are indistinguishable there.
        // Printing them here is how we learn which ones real plugins send.
        if( gTrace ) std::printf( "    edit   : restartComponent flags=0x%x\n", (unsigned)flags );
        return kResultOk;
    }

    int             edits            = 0;
    int             begins           = 0;
    int             performs         = 0;
    int             ends             = 0;
    int             restarts         = 0;
    int32           lastRestartFlags = 0;
    Vst::ParamID    lastId           = 0;
    Vst::ParamValue lastValue        = 0.0;
};

// --- module loading -----------------------------------------------------------
//
// A .vst3 is a BUNDLE on every platform in the 3.6.10+ layout, but Windows also
// still allows a plain DLL renamed to .vst3 — and both are in the wild, exactly
// like the flat-vs-bundle .clap split that broke the CLAP loader on macOS in M7.
// Getting this wrong is not an ABI question, so the loader resolves it first and
// prints what it decided.
class Vst3Module {
public:
    ~Vst3Module() { unload(); }

    bool load( const std::string &path )
    {
        const std::string binary = resolveBinary( path );
        if( binary.empty() ) {
            std::printf( "  FAILED: no loadable binary inside '%s'\n", path.c_str() );
            return false;
        }
        std::printf( "  binary : %s\n", binary.c_str() );

#if defined( _WIN32 )
        // ALTERED_SEARCH_PATH so a plugin's sibling DLLs resolve from its own
        // directory — the usual reason a perfectly good plugin "fails to load".
        const std::wstring w = widen( binary );
        handle_ = (void *)LoadLibraryExW( w.c_str(), nullptr,
                                          LOAD_WITH_ALTERED_SEARCH_PATH );
        if( !handle_ ) {
            std::printf( "  FAILED: LoadLibraryExW error %lu\n",
                         (unsigned long)GetLastError() );
            return false;
        }
        const char *initName = "InitDll";
        const char *exitName = "ExitDll";
#elif defined( __APPLE__ )
        handle_ = dlopen( binary.c_str(), RTLD_LOCAL | RTLD_LAZY );
        if( !handle_ ) {
            std::printf( "  FAILED: dlopen: %s\n", dlerror() );
            return false;
        }
        const char *initName = "bundleEntry";
        const char *exitName = "bundleExit";
#else
        handle_ = dlopen( binary.c_str(), RTLD_LOCAL | RTLD_LAZY );
        if( !handle_ ) {
            std::printf( "  FAILED: dlopen: %s\n", dlerror() );
            return false;
        }
        const char *initName = "ModuleEntry";
        const char *exitName = "ModuleExit";
#endif

        exitName_ = exitName;

        // The entry point is OPTIONAL in the spec but universal in practice; a
        // plugin that has one and does not get it called will usually crash
        // later rather than here, so a miss is worth reporting.
        void *entry = symbol( initName );
        if( entry ) {
#if defined( _WIN32 )
            inited_ = ( (bool ( * )())entry )();
#else
            inited_ = ( (bool ( * )( void * ))entry )( handle_ );
#endif
            if( !inited_ ) {
                std::printf( "  FAILED: %s() returned false\n", initName );
                unload();
                return false;
            }
        } else {
            std::printf( "  note   : no %s export (allowed, but unusual)\n", initName );
        }

        void *getFactory = symbol( "GetPluginFactory" );
        if( !getFactory ) {
            std::printf( "  FAILED: no GetPluginFactory export\n" );
            unload();
            return false;
        }

        factory_ = ( (IPluginFactory * ( * )())getFactory )();
        if( !factory_ ) {
            std::printf( "  FAILED: GetPluginFactory() returned null\n" );
            unload();
            return false;
        }
        return true;
    }

    IPluginFactory *factory() const { return factory_; }

private:
    // The per-platform architecture folder inside a bundle.
    static std::vector<std::string> archDirs()
    {
#if defined( _WIN32 )
#if defined( __aarch64__ ) || defined( _M_ARM64 )
        return { "arm64ec-win", "arm64-win", "x86_64-win" };
#else
        return { "x86_64-win", "x86_64-win_arm64ec" };
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

    static std::string resolveBinary( const std::string &path )
    {
        // A plain file: Windows' legacy "DLL renamed to .vst3". Nothing to
        // resolve.
        if( fileExists( path ) ) return path;
        if( !isDirectory( path ) ) return std::string();

        const std::string base = baseName( path );
        // Foo.vst3 -> the inner binary is named Foo.vst3 on Windows, Foo on
        // macOS/Linux. Try the conventional name first, then any regular file
        // in the arch dir — the same name-mismatch fallback the CLAP loader
        // needed in M7.
        std::string stem = base;
        const std::size_t dot = stem.find_last_of( '.' );
        if( dot != std::string::npos ) stem = stem.substr( 0, dot );

        for( const std::string &arch : archDirs() ) {
            const std::string dir = path + "/Contents/" + arch;
            if( !isDirectory( dir ) ) continue;
            for( const std::string &cand : { base, stem, stem + ".so" } ) {
                const std::string full = dir + "/" + cand;
                if( fileExists( full ) ) return full;
            }
            const std::string sole = soleFileIn( dir );
            if( !sole.empty() ) return sole;
        }
        return std::string();
    }

    static std::string soleFileIn( const std::string &dir )
    {
        std::vector<std::string> found;
#if defined( _WIN32 )
        WIN32_FIND_DATAA fd;
        const HANDLE h = FindFirstFileA( ( dir + "\\*" ).c_str(), &fd );
        if( h == INVALID_HANDLE_VALUE ) return std::string();
        do {
            if( !( fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) )
                found.push_back( dir + "/" + fd.cFileName );
        } while( FindNextFileA( h, &fd ) );
        FindClose( h );
#else
        if( DIR *d = ::opendir( dir.c_str() ) ) {
            while( struct dirent *e = ::readdir( d ) ) {
                const std::string name = e->d_name;
                if( name == "." || name == ".." ) continue;
                const std::string full = dir + "/" + name;
                if( fileExists( full ) ) found.push_back( full );
            }
            ::closedir( d );
        }
#endif
        return found.size() == 1 ? found.front() : std::string();
    }

    void *symbol( const char *name ) const
    {
        if( !handle_ ) return nullptr;
#if defined( _WIN32 )
        return (void *)GetProcAddress( (HMODULE)handle_, name );
#else
        return dlsym( handle_, name );
#endif
    }

#if defined( _WIN32 )
    static std::wstring widen( const std::string &s )
    {
        const int n = MultiByteToWideChar( CP_UTF8, 0, s.c_str(), -1, nullptr, 0 );
        std::wstring w( n > 0 ? n - 1 : 0, L'\0' );
        if( n > 0 ) MultiByteToWideChar( CP_UTF8, 0, s.c_str(), -1, &w[0], n );
        return w;
    }
#endif

    void unload()
    {
        if( factory_ ) {
            factory_->release();
            factory_ = nullptr;
        }
        if( handle_ ) {
            if( inited_ && exitName_ ) {
                if( void *e = symbol( exitName_ ) )
                    ( (bool ( * )())e )();
            }
#if defined( _WIN32 )
            FreeLibrary( (HMODULE)handle_ );
#else
            dlclose( handle_ );
#endif
            handle_ = nullptr;
        }
        inited_ = false;
    }

    void           *handle_   = nullptr;
    IPluginFactory *factory_  = nullptr;
    const char     *exitName_ = nullptr;
    bool            inited_   = false;
};

// --- the native editor view (proposal 33 PoC, --view) -------------------------
//
// The GUI half of the ABI question, and the one proposal 33 named as its
// riskiest VST3 unknown: a MinGW-built host hands an HWND it owns to an
// MSVC-built plugin's IPlugView::attached(). vst3_probe already proved the
// AUDIO interfaces cross that boundary; IPlugView is the same COM shape, but
// "the same shape" is a prediction until something runs it.
//
// Deliberately off screen. WS_POPUP with no WS_VISIBLE gives a real, valid HWND
// with a real window station and message queue — everything attached() needs —
// without a window appearing on the user's desktop during an unattended sweep.
// A plugin that only paints on WM_PAINT simply never gets one, which does not
// affect a single call below.

#if defined( _WIN32 )
// One container per view, destroyed with it. A class registered once.
// Hidden by default; --show makes it a real visible window (see runShowLoop).
HWND makeHostWindow()
{
    static bool registered = false;
    static const wchar_t *kClass = L"SmaragdVst3ProbeHost";
    if( !registered ) {
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof( wc );
        wc.lpfnWndProc   = DefWindowProcW;
        wc.hInstance     = GetModuleHandleW( nullptr );
        wc.lpszClassName = kClass;
        if( !RegisterClassExW( &wc ) ) {
            warn( "RegisterClassExW failed (%lu)", (unsigned long)GetLastError() );
            return nullptr;
        }
        registered = true;
    }
    // --show gives a real framed window the user can see, move and close;
    // otherwise a bare WS_POPUP that is never shown. Both are equally valid
    // parents as far as attached() is concerned — the difference is only
    // whether a human can look at the result.
    const DWORD style = gShow ? ( WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX )
                              : WS_POPUP;
    HWND h = CreateWindowExW( 0, kClass, L"vst3_probe — plugin editor", style,
                              CW_USEDEFAULT, CW_USEDEFAULT, 800, 600,
                              nullptr, nullptr, GetModuleHandleW( nullptr ), nullptr );
    if( !h ) warn( "CreateWindowExW failed (%lu)", (unsigned long)GetLastError() );
    return h;
}

// Size the frame so its CLIENT area is exactly the plugin's reported size.
// IPlugView sizes are PHYSICAL PIXELS on Windows (iplugview.h:98-100) and a
// window's outer rect includes the caption and borders, so handing the plugin's
// number straight to MoveWindow crops the GUI by the frame — the commonest
// visible symptom of getting this wrong.
void fitClientTo( HWND h, int w, int h_px )
{
    RECT r{ 0, 0, w, h_px };
    AdjustWindowRectEx( &r, (DWORD)GetWindowLongPtrW( h, GWL_STYLE ), FALSE,
                        (DWORD)GetWindowLongPtrW( h, GWL_EXSTYLE ) );
    SetWindowPos( h, nullptr, 0, 0, r.right - r.left, r.bottom - r.top,
                  SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE );
}

// The real-life test. Shows the window and runs an ACTUAL message loop, so the
// plugin paints, takes mouse and keyboard, and — the point of the exercise —
// reports parameter edits back through IComponentHandler while a human turns a
// knob. Ends when the user closes the window or the budget expires.
void runShowLoop( HWND host, ComponentHandler &handler, int seconds )
{
    ShowWindow( host, SW_SHOWNORMAL );
    UpdateWindow( host );
    SetForegroundWindow( host );

    std::printf( "    show   : window is up for up to %d s — TURN A KNOB, then close it\n",
                 seconds );
    std::fflush( stdout );

    const DWORD deadline = GetTickCount() + (DWORD)( seconds * 1000 );
    MSG msg;
    while( IsWindow( host ) && GetTickCount() < deadline ) {
        while( PeekMessageW( &msg, nullptr, 0, 0, PM_REMOVE ) ) {
            if( msg.message == WM_QUIT ) return;
            TranslateMessage( &msg );
            DispatchMessageW( &msg );
        }
        Sleep( 10 );
        std::fflush( stdout );
    }

    std::printf( "    show   : closed after %d begin / %d perform / %d end\n",
                 handler.begins, handler.performs, handler.ends );
}

// A plugin's view may post to itself during attached(); a host that never pumps
// can leave it half-constructed. Bounded, because a spike must not hang.
void pumpMessages( int iterations )
{
    MSG msg;
    for( int i = 0; i < iterations; ++i )
        while( PeekMessageW( &msg, nullptr, 0, 0, PM_REMOVE ) ) {
            TranslateMessage( &msg );
            DispatchMessageW( &msg );
        }
}
#endif

// Returns false only on a hard ABI failure (a call that should have worked and
// did not). "This plugin has no editor" is a legitimate, reported outcome.
bool probeView( Vst::IEditController *controller, ComponentHandler &handler )
{
    if( !controller ) {
        std::printf( "    view   : no controller — nothing to ask for a view\n" );
        return true;
    }

    IPlugView *view = controller->createView( Vst::ViewType::kEditor );
    if( !view ) {
        std::printf( "    view   : createView(kEditor) -> null (plugin has no editor)\n" );
        return true;
    }
    std::printf( "    view   : createView(kEditor) -> %p\n", (void *)view );

    bool ok = true;

#if defined( _WIN32 )
    const FIDString kPlatform = kPlatformTypeHWND;
    const char     *kPlatName = "HWND";
#elif defined( __APPLE__ )
    const FIDString kPlatform = kPlatformTypeNSView;
    const char     *kPlatName = "NSView";
#else
    const FIDString kPlatform = kPlatformTypeX11EmbedWindowID;
    const char     *kPlatName = "X11EmbedWindowID";
#endif

    const tresult supported = view->isPlatformTypeSupported( kPlatform );
    std::printf( "    view   : isPlatformTypeSupported(%s) -> %s\n", kPlatName,
                 supported == kResultTrue ? "yes" : "NO" );

    // Size BEFORE attach. Many plugins answer here; some only after attached().
    ViewRect r0{};
    const tresult gs0 = view->getSize( &r0 );
    std::printf( "    view   : getSize (pre-attach)  -> %s %dx%d\n",
                 gs0 == kResultOk ? "ok" : "FAILED", r0.getWidth(), r0.getHeight() );

    std::printf( "    view   : canResize -> %s\n",
                 view->canResize() == kResultTrue ? "yes" : "no" );

    // The content-scale leg (HiDPI). Optional; a plugin that does not implement
    // it is not a failure, it just wants physical pixels at scale 1.
    IPlugViewContentScaleSupport *scale = nullptr;
    if( view->queryInterface( IPlugViewContentScaleSupport::iid, (void **)&scale ) == kResultOk
        && scale ) {
        const tresult sr = scale->setContentScaleFactor( 1.0f );
        std::printf( "    view   : IPlugViewContentScaleSupport -> yes, setScale(1.0) %s\n",
                     sr == kResultOk ? "ok" : "refused" );
        scale->release();
    } else {
        std::printf( "    view   : IPlugViewContentScaleSupport -> not offered\n" );
    }

    // setFrame BEFORE attached(): the plugin may call resizeView from inside
    // attached(), and a null frame at that moment silently loses the request.
    PlugFrame frame;
    const tresult sf = view->setFrame( &frame );
    std::printf( "    view   : setFrame -> %s\n",
                 sf == kResultOk ? "ok" : ( sf == kNotImplemented ? "kNotImplemented" : "FAILED" ) );

    if( supported != kResultTrue ) {
        std::printf( "    view   : platform unsupported — not attaching\n" );
        view->setFrame( nullptr );
        view->release();
        return ok;
    }

#if defined( _WIN32 )
    HWND host = makeHostWindow();
    if( !host ) {
        view->setFrame( nullptr );
        view->release();
        return false;
    }

    // THE MEASUREMENT. A MinGW HWND into an MSVC plugin's attached().
    const tresult at = view->attached( (void *)host, kPlatform );
    std::printf( "    view   : attached(HWND %p) -> %s\n", (void *)host,
                 at == kResultOk ? "OK" : "FAILED" );
    if( at != kResultOk ) {
        warn( "attached() refused the host window — SUSPECT GUI ABI or platform type" );
        ok = false;
    } else {
        pumpMessages( 4 );

        ViewRect r1{};
        if( view->getSize( &r1 ) == kResultOk )
            std::printf( "    view   : getSize (post-attach) -> %dx%d\n",
                         r1.getWidth(), r1.getHeight() );

        // Did the plugin actually create a child window inside ours? This is the
        // difference between "attached() returned kResultOk" and "the GUI is
        // really in our window", and only the second one is worth anything.
        HWND child = GetWindow( host, GW_CHILD );
        if( child ) {
            RECT cr{};
            GetWindowRect( child, &cr );
            std::printf( "    view   : child HWND %p created, %ldx%ld  [EMBEDDING CONFIRMED]\n",
                         (void *)child, cr.right - cr.left, cr.bottom - cr.top );
        } else {
            std::printf( "    view   : no child HWND — plugin drew into ours directly, "
                         "or deferred creation\n" );
        }

        // Host-driven resize, only where the plugin allows it.
        if( view->canResize() == kResultTrue && r1.getWidth() > 0 ) {
            ViewRect want{ 0, 0, r1.getWidth() + 40, r1.getHeight() + 40 };
            const tresult cs = view->checkSizeConstraint( &want );
            std::printf( "    view   : checkSizeConstraint(%dx%d) -> %s, gives %dx%d\n",
                         r1.getWidth() + 40, r1.getHeight() + 40,
                         cs == kResultTrue ? "ok" : "adjusted",
                         want.getWidth(), want.getHeight() );
            const tresult os = view->onSize( &want );
            std::printf( "    view   : onSize -> %s\n", os == kResultOk ? "ok" : "refused" );
            pumpMessages( 2 );
        }

        std::printf( "    view   : IPlugFrame::resizeView called %d time(s)%s\n",
                     frame.resizes,
                     frame.resizes ? "  [REVERSE GUI ABI CONFIRMED]" : "" );

        // --- the real-life test (proposal 33 §4, the one gap a human closes) --
        if( gShow ) {
            ViewRect cur{};
            if( view->getSize( &cur ) == kResultOk && cur.getWidth() > 0 )
                fitClientTo( host, cur.getWidth(), cur.getHeight() );

            const int before = handler.edits;
            runShowLoop( host, handler, gShowSeconds );

            if( handler.edits == before ) {
                std::printf( "    show   : NO component-handler traffic at all\n" );
            } else {
                std::printf( "    show   : last edit id=%u value=%.6f\n",
                             (unsigned)handler.lastId, handler.lastValue );
                std::printf( "    show   : %s\n",
                             ( handler.begins > 0 && handler.ends > 0 )
                                 ? "GESTURES ARE BRACKETED — begin/end usable as a punch-in"
                                 : "values arrive but NOT bracketed — no gesture to punch in on" );
            }
        }
    }

    // Teardown order matters: removed() BEFORE release(), and the frame must
    // outlive both. Getting this wrong is a crash inside the plugin, not a
    // return code, which is precisely why the spike does it before the backend.
    const tresult rm = view->removed();
    std::printf( "    view   : removed -> %s\n", rm == kResultOk ? "ok" : "FAILED" );
    view->setFrame( nullptr );
    view->release();
    DestroyWindow( host );
    pumpMessages( 2 );
#else
    std::printf( "    view   : attach not implemented in this spike on this platform\n" );
    view->setFrame( nullptr );
    view->release();
#endif

    std::printf( "    view   : survived create -> attach -> resize -> removed -> release\n" );
    return ok;
}

// --- the actual probe ---------------------------------------------------------

constexpr int32 kBlock = 512;
constexpr double kRate = 48000.0;

// Instantiate one audio-effect class and walk it through the whole lifecycle the
// engine will use. Returns true if it survived intact.
bool probeClass( IPluginFactory *factory, const PClassInfo &ci, HostApp &host,
                 ComponentHandler &handler )
{
    Vst::IComponent *component = nullptr;
    tresult r = factory->createInstance( ci.cid, Vst::IComponent::iid,
                                         (void **)&component );
    if( r != kResultOk || !component ) {
        warn( "createInstance(IComponent) -> %s", resultName( r ) );
        return false;
    }

    bool ok = true;

    r = component->initialize( &host );
    if( r != kResultOk ) {
        warn( "IComponent::initialize -> %s", resultName( r ) );
        component->release();
        return false;
    }

    // --- buses ---------------------------------------------------------------
    const int32 nIn  = component->getBusCount( Vst::kAudio, Vst::kInput );
    const int32 nOut = component->getBusCount( Vst::kAudio, Vst::kOutput );
    const int32 nEvIn = component->getBusCount( Vst::kEvent, Vst::kInput );
    std::printf( "    buses  : %d audio in, %d audio out, %d event in\n",
                 (int)nIn, (int)nOut, (int)nEvIn );

    int32 mainInCh = 0, mainOutCh = 0;
    for( int32 dir = 0; dir < 2; ++dir ) {
        const Vst::BusDirection d = dir == 0 ? Vst::kInput : Vst::kOutput;
        const int32 n = dir == 0 ? nIn : nOut;
        for( int32 i = 0; i < n; ++i ) {
            Vst::BusInfo bi{};
            if( component->getBusInfo( Vst::kAudio, d, i, bi ) != kResultOk ) {
                warn( "getBusInfo(%s,%d) failed", dir == 0 ? "in" : "out", (int)i );
                continue;
            }
            std::printf( "      %-3s [%d] %-28s %d ch  %s%s\n",
                         dir == 0 ? "in" : "out", (int)i,
                         u16ToUtf8( bi.name ).c_str(), (int)bi.channelCount,
                         bi.busType == Vst::kMain ? "main" : "aux",
                         ( bi.flags & Vst::BusInfo::kDefaultActive ) ? ", default-active" : "" );
            if( bi.busType == Vst::kMain && i == 0 ) {
                if( dir == 0 ) mainInCh = bi.channelCount;
                else           mainOutCh = bi.channelCount;
            }
            // A struct-layout mismatch across the ABI shows up here first: a
            // channel count in the thousands means BusInfo is being read at the
            // wrong offsets, and every later result is meaningless.
            if( bi.channelCount < 0 || bi.channelCount > 64 ) {
                warn( "implausible channelCount %d — SUSPECT STRUCT LAYOUT MISMATCH",
                      (int)bi.channelCount );
                ok = false;
            }
        }
    }

    // --- processor -----------------------------------------------------------
    Vst::IAudioProcessor *proc = nullptr;
    r = component->queryInterface( Vst::IAudioProcessor::iid, (void **)&proc );
    if( r != kResultOk || !proc ) {
        warn( "queryInterface(IAudioProcessor) -> %s", resultName( r ) );
        component->terminate();
        component->release();
        return false;
    }

    const bool f32 = proc->canProcessSampleSize( Vst::kSample32 ) == kResultTrue;
    std::printf( "    sample : float32 %s, latency %u frames\n",
                 f32 ? "yes" : "NO", (unsigned)proc->getLatencySamples() );
    if( !f32 ) {
        warn( "plugin refuses kSample32; the engine is float32-only" );
        ok = false;
    }

    // Ask for the layout the engine actually wants before setupProcessing —
    // some plugins report a different bus arrangement afterwards.
    if( nIn > 0 || nOut > 0 ) {
        Vst::SpeakerArrangement in  = mainInCh  == 1 ? Vst::SpeakerArr::kMono : Vst::SpeakerArr::kStereo;
        Vst::SpeakerArrangement out = mainOutCh == 1 ? Vst::SpeakerArr::kMono : Vst::SpeakerArr::kStereo;
        r = proc->setBusArrangements( nIn > 0 ? &in : nullptr, nIn > 0 ? 1 : 0,
                                      nOut > 0 ? &out : nullptr, nOut > 0 ? 1 : 0 );
        std::printf( "    arrange: setBusArrangements -> %s\n", resultName( r ) );
    }

    Vst::ProcessSetup setup{};
    setup.processMode         = Vst::kRealtime;
    setup.symbolicSampleSize  = Vst::kSample32;
    setup.maxSamplesPerBlock  = kBlock;
    setup.sampleRate          = kRate;
    r = proc->setupProcessing( setup );
    if( r != kResultOk ) {
        warn( "setupProcessing -> %s", resultName( r ) );
        ok = false;
    }

    for( int32 i = 0; i < nIn; ++i )
        component->activateBus( Vst::kAudio, Vst::kInput, i, true );
    for( int32 i = 0; i < nOut; ++i )
        component->activateBus( Vst::kAudio, Vst::kOutput, i, true );

    // --- controller ----------------------------------------------------------
    //
    // Two shapes exist: a single-component plugin whose IComponent also answers
    // IEditController, and the usual split pair reached through
    // getControllerClassId. M6 must handle both; so does the probe.
    Vst::IEditController *controller = nullptr;
    bool controllerIsSeparate = false;
    if( component->queryInterface( Vst::IEditController::iid, (void **)&controller ) != kResultOk )
        controller = nullptr;
    if( !controller ) {
        TUID cid;
        if( component->getControllerClassId( cid ) == kResultOk ) {
            if( factory->createInstance( cid, Vst::IEditController::iid,
                                         (void **)&controller ) == kResultOk && controller ) {
                controllerIsSeparate = true;
                if( controller->initialize( &host ) != kResultOk )
                    warn( "IEditController::initialize failed" );
            }
        }
    }

    if( controller ) {
        controller->setComponentHandler( &handler );

        // Hand the component's state to the controller, as a host must.
        MemStream compState;
        if( component->getState( &compState ) == kResultOk ) {
            compState.rewind();
            controller->setComponentState( &compState );
        }

        // Connect the pair. Without this, parameter edits made in one half never
        // reach the other — the single most common host bug.
        if( controllerIsSeparate ) {
            Vst::IConnectionPoint *cpComp = nullptr, *cpCtrl = nullptr;
            component->queryInterface( Vst::IConnectionPoint::iid, (void **)&cpComp );
            controller->queryInterface( Vst::IConnectionPoint::iid, (void **)&cpCtrl );
            if( cpComp && cpCtrl ) {
                const tresult a = cpComp->connect( cpCtrl );
                const tresult b = cpCtrl->connect( cpComp );
                std::printf( "    connect: component->controller %s, controller->component %s\n",
                             resultName( a ), resultName( b ) );
            } else {
                std::printf( "    connect: no IConnectionPoint on %s\n",
                             cpComp ? "controller" : "component" );
            }
            if( cpComp ) cpComp->release();
            if( cpCtrl ) cpCtrl->release();
        }

        const int32 nParams = controller->getParameterCount();
        std::printf( "    params : %d%s\n", (int)nParams,
                     controllerIsSeparate ? "  (separate controller)" : "  (single component)" );
        for( int32 i = 0; i < nParams && i < 8; ++i ) {
            Vst::ParameterInfo pi{};
            if( controller->getParameterInfo( i, pi ) != kResultOk ) continue;
            std::printf( "      [%u] %-28s steps %d, default %.3f%s\n",
                         (unsigned)pi.id, u16ToUtf8( pi.title ).c_str(),
                         (int)pi.stepCount, (double)pi.defaultNormalizedValue,
                         ( pi.flags & Vst::ParameterInfo::kCanAutomate ) ? ", automatable" : "" );
        }
        if( nParams > 8 ) std::printf( "      ... %d more\n", (int)( nParams - 8 ) );
    } else {
        std::printf( "    params : no IEditController\n" );
    }

    // --- state round trip ----------------------------------------------------
    //
    // Runs the plugin through OUR IBStream vtable in both directions. A crash
    // here, with a clean call table above it, would point at our host object
    // rather than at the plugin.
    MemStream state;
    r = component->getState( &state );
    if( r == kResultOk ) {
        state.rewind();
        const tresult back = component->setState( &state );
        std::printf( "    state  : %zu bytes, reloads: %s\n", state.size(),
                     back == kResultOk ? "yes" : resultName( back ) );
    } else {
        std::printf( "    state  : getState -> %s\n", resultName( r ) );
    }

    // --- one real process() block --------------------------------------------
    //
    // The end-to-end ABI proof: ProcessData and AudioBusBuffers cross the
    // boundary by value/pointer, and the plugin writes into buffers we own. A
    // half-scale impulse in, so a pure pass-through is distinguishable from
    // silence and from an untouched buffer.
    if( ok && f32 ) {
        r = component->setActive( true );
        if( r != kResultOk ) {
            warn( "setActive(true) -> %s", resultName( r ) );
            ok = false;
        } else {
            proc->setProcessing( true );

            const int32 inCh  = std::max<int32>( mainInCh, 0 );
            const int32 outCh = std::max<int32>( mainOutCh, 0 );

            std::vector<std::vector<float>> inBuf( (std::size_t)std::max( inCh, 1 ) );
            std::vector<std::vector<float>> outBuf( (std::size_t)std::max( outCh, 1 ) );
            std::vector<float *> inPtr, outPtr;
            for( auto &v : inBuf ) {
                v.assign( kBlock, 0.0f );
                for( int32 i = 0; i < kBlock; ++i )
                    v[(std::size_t)i] = 0.5f * ( ( i % 64 ) < 32 ? 1.0f : -1.0f );
                inPtr.push_back( v.data() );
            }
            for( auto &v : outBuf ) {
                v.assign( kBlock, -777.0f );   // poison: proves the plugin wrote
                outPtr.push_back( v.data() );
            }

            Vst::AudioBusBuffers inBus{}, outBus{};
            inBus.numChannels       = inCh;
            inBus.channelBuffers32  = inPtr.data();
            outBus.numChannels      = outCh;
            outBus.channelBuffers32 = outPtr.data();

            Vst::ProcessData data{};
            data.processMode        = Vst::kRealtime;
            data.symbolicSampleSize = Vst::kSample32;
            data.numSamples         = kBlock;
            data.numInputs          = nIn > 0 ? 1 : 0;
            data.numOutputs         = nOut > 0 ? 1 : 0;
            data.inputs             = nIn > 0 ? &inBus : nullptr;
            data.outputs            = nOut > 0 ? &outBus : nullptr;

            r = proc->process( data );
            if( r != kResultOk ) {
                warn( "process -> %s", resultName( r ) );
                ok = false;
            } else if( outCh > 0 ) {
                double peak = 0.0, sum = 0.0;
                int    untouched = 0, nonFinite = 0;
                for( int32 c = 0; c < outCh; ++c ) {
                    for( int32 i = 0; i < kBlock; ++i ) {
                        const float s = outBuf[(std::size_t)c][(std::size_t)i];
                        if( s == -777.0f ) ++untouched;
                        if( !( s == s ) || s > 1e6f || s < -1e6f ) ++nonFinite;
                        peak = std::max( peak, (double)( s < 0 ? -s : s ) );
                        sum += (double)s * (double)s;
                    }
                }
                const double rms = std::sqrt( sum / (double)( outCh * kBlock ) );
                std::printf( "    process: ok, out peak %.4f rms %.4f%s%s\n", peak, rms,
                             untouched ? "  [SOME SAMPLES UNTOUCHED]" : "",
                             nonFinite ? "  [NON-FINITE SAMPLES]" : "" );
                if( nonFinite ) {
                    warn( "non-finite output — SUSPECT ProcessData LAYOUT MISMATCH" );
                    ok = false;
                }
                if( untouched == outCh * kBlock )
                    std::printf( "    note   : output buffer never written (plugin may need "
                                 "parameters, notes, or more blocks)\n" );
            }

            proc->setProcessing( false );
            component->setActive( false );
        }
    }

    // --- native editor (proposal 33 PoC) --------------------------------------
    //
    // AFTER process() and BEFORE teardown, on purpose: a real host opens an
    // editor on a plugin that is already set up and possibly running, and a view
    // must be gone before setComponentHandler(nullptr).
    if( gWantView && !probeView( controller, handler ) ) ok = false;

    // --- teardown ------------------------------------------------------------
    //
    // In the documented order. Getting it wrong is how a host leaks a DSO or
    // crashes on exit, and the spike should prove the order works before M6
    // encodes it.
    if( controller ) {
        controller->setComponentHandler( nullptr );
        if( controllerIsSeparate ) controller->terminate();
        controller->release();
    }
    proc->release();
    component->terminate();
    component->release();

    return ok;
}

int probeOne( const std::string &path )
{
    std::printf( "=== %s\n", path.c_str() );

    Vst3Module mod;
    if( !mod.load( path ) ) return 1;

    IPluginFactory *factory = mod.factory();

    PFactoryInfo fi{};
    if( factory->getFactoryInfo( &fi ) == kResultOk )
        std::printf( "  vendor : %s  <%s>  %s\n",
                     fixedStr( fi.vendor, PFactoryInfo::kNameSize ).c_str(),
                     fixedStr( fi.email, PFactoryInfo::kEmailSize ).c_str(),
                     fixedStr( fi.url, PFactoryInfo::kURLSize ).c_str() );

    PlugInterfaceSupport support;
    HostApp              host( &support );
    ComponentHandler     handler;

    // Which factory revision the plugin offers tells us how much class metadata
    // (subCategories, and therefore instrument-vs-effect) we can trust.
    IPluginFactory2 *f2 = nullptr;
    IPluginFactory3 *f3 = nullptr;
    factory->queryInterface( IPluginFactory2::iid, (void **)&f2 );
    factory->queryInterface( IPluginFactory3::iid, (void **)&f3 );
    std::printf( "  factory: IPluginFactory%s%s\n", f2 ? " + 2" : "", f3 ? " + 3" : "" );
    if( f3 ) {
        // A factory3 wants the host context BEFORE any class is created; some
        // plugins refuse to instantiate without it.
        const tresult hc = f3->setHostContext( &host );
        std::printf( "  factory: setHostContext -> %s\n", resultName( hc ) );
    }

    const int32 nClasses = factory->countClasses();
    std::printf( "  classes: %d\n", (int)nClasses );

    int effects = 0, good = 0;
    for( int32 i = 0; i < nClasses; ++i ) {
        PClassInfo ci{};
        if( factory->getClassInfo( i, &ci ) != kResultOk ) {
            std::printf( "  - [%d] getClassInfo failed\n", (int)i );
            continue;
        }

        std::string sub, version;
        if( f2 ) {
            PClassInfo2 ci2{};
            if( f2->getClassInfo2( i, &ci2 ) == kResultOk ) {
                sub     = fixedStr( ci2.subCategories, PClassInfo2::kSubCategoriesSize );
                version = fixedStr( ci2.version, PClassInfo2::kVersionSize );
            }
        }

        const std::string name     = fixedStr( ci.name, PClassInfo::kNameSize );
        const std::string category = fixedStr( ci.category, PClassInfo::kCategorySize );

        std::printf( "  - [%d] %s\n", (int)i, name.c_str() );
        std::printf( "    cid    : %s\n", tuidToHex( ci.cid ).c_str() );
        std::printf( "    class  : %s", category.c_str() );
        if( !sub.empty() )     std::printf( "  sub=%s", sub.c_str() );
        if( !version.empty() ) std::printf( "  v%s", version.c_str() );
        std::printf( "\n" );

        if( category != kVstAudioEffectClass ) {
            std::printf( "    (not an audio module class; skipped)\n" );
            continue;
        }
        ++effects;

        // An instrument is an audio-effect class whose subCategories say so —
        // there is no separate category for it. Out of scope until the MIDI/note
        // model exists, but worth surfacing here.
        if( sub.find( "Instrument" ) != std::string::npos )
            std::printf( "    note   : declares Instrument — out of scope until the note model exists\n" );

        if( probeClass( factory, ci, host, handler ) ) ++good;
    }

    std::printf( "  host   : %d createInstance request(s), %d component-handler edit(s) "
                 "(%d begin / %d perform / %d end), %d restart(s)\n",
                 host.createInstanceRequests, handler.edits,
                 handler.begins, handler.performs, handler.ends, handler.restarts );

    if( f2 ) f2->release();
    if( f3 ) f3->release();

    if( effects == 0 ) {
        std::printf( "  RESULT : loaded, but no audio-effect class\n" );
        return 1;
    }
    std::printf( "  RESULT : %d/%d audio-effect class(es) survived the full lifecycle\n",
                 good, effects );
    return good == effects ? 0 : 1;
}

}  // namespace

int main( int argc, char **argv )
{
#if defined( _WIN32 )
    // Never answer a malformed module with a modal dialog — the same call
    // clap_probe.cc and tools/plugin_probe.cc make, for the same reason: a spike
    // run over a directory of unknown files must stay unattended.
    SetErrorMode( SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX
                  | SEM_NOOPENFILEERRORBOX );
#endif

    int firstPath = 1;
    for( ; firstPath < argc; ++firstPath ) {
        const char *a = argv[firstPath];
        if( std::strcmp( a, "--view" ) == 0 )        gWantView = true;
        else if( std::strcmp( a, "--trace" ) == 0 )  gTrace = true;
        else if( std::strcmp( a, "--show" ) == 0 ) {
            // --show implies --view (there is nothing to show otherwise) and
            // --trace (the point is watching the callbacks land live).
            gShow = gWantView = gTrace = true;
        } else if( std::strncmp( a, "--seconds=", 10 ) == 0 ) {
            gShowSeconds = std::atoi( a + 10 );
            if( gShowSeconds < 1 )   gShowSeconds = 1;
            if( gShowSeconds > 600 ) gShowSeconds = 600;
        } else break;
    }

    if( firstPath >= argc ) {
        std::printf( "usage: %s [--view] [--show] [--trace] [--seconds=N] "
                     "<plugin.vst3> [more.vst3 ...]\n",
                     argc > 0 ? argv[0] : "vst3_probe" );
        std::printf( "\n"
                     "Proposal 08 M6 ABI gate: loads an MSVC-built VST3 from this\n"
                     "MinGW-built host and walks it through instantiate -> initialize ->\n"
                     "buses -> params -> state -> process -> teardown.\n"
                     "\n"
                     "  --view       also walk the NATIVE EDITOR (proposal 33): createView ->\n"
                     "               setFrame -> attached() into a real off-screen HWND ->\n"
                     "               getSize/canResize/onSize -> removed -> release, reporting\n"
                     "               whether the plugin created a child window and whether it\n"
                     "               called IPlugFrame::resizeView back into us.\n"
                     "\n"
                     "  --show       THE REAL-LIFE TEST. Implies --view and --trace. Makes the\n"
                     "               host window VISIBLE, sizes its client area to the plugin's\n"
                     "               reported size and runs a real message loop, so the GUI\n"
                     "               paints and takes input. Turn a knob: every beginEdit /\n"
                     "               performEdit / endEdit is printed with its ParamID and\n"
                     "               value. That is the one thing no headless run can measure,\n"
                     "               and proposal 33 M2 depends on the answer.\n"
                     "\n"
                     "  --trace      print component-handler callbacks as they arrive.\n"
                     "  --seconds=N  how long --show stays up (default 30, max 600).\n" );
        return 2;
    }

    int rc = 0;
    for( int i = firstPath; i < argc; ++i ) {
        rc |= probeOne( argv[i] );
        std::printf( "\n" );
    }

    if( gWarnings )
        std::printf( "%d warning(s) — see the '!' lines above.\n", gWarnings );
    std::printf( "%s\n", rc == 0 ? "GATE PASSED: the MinGW <-> MSVC VST3 ABI works here."
                                 : "GATE FAILED: see above." );
    return rc;
}
