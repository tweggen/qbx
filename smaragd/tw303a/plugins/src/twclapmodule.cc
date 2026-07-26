#include "twclapmodule.h"

#include "tw/core/twlog.h"

#include <map>
#include <mutex>

#if defined( _WIN32 )
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace audio {

namespace {

// --- module interning ------------------------------------------------------
// Guards gModules. A plain function-local static: the map outlives every plugin
// instance by construction (entries are weak), and it is only ever touched off
// the audio thread (open() loads a DSO; the destructor unloads one).
std::mutex &moduleMutex()
{
    static std::mutex m;
    return m;
}

std::map<std::string, std::weak_ptr<twClapModule>> &moduleTable()
{
    static std::map<std::string, std::weak_ptr<twClapModule>> t;
    return t;
}

#if defined( _WIN32 )
std::wstring widen( const std::string &s )
{
    if( s.empty() )
        return std::wstring();
    int n = MultiByteToWideChar( CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0 );
    if( n <= 0 )
        return std::wstring();
    std::wstring w( (size_t)n, L'\0' );
    MultiByteToWideChar( CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n );
    return w;
}
#endif

#if defined( __APPLE__ )
// A macOS .clap is a bundle directory; the DSO lives at
// <bundle>/Contents/MacOS/<basename-without-extension>. CFBundle is not needed
// for a plain dlopen of the executable, and avoiding it keeps this file free of
// CoreFoundation (tw_plugins links neither).
std::string macBundleBinary( const std::string &bundlePath )
{
    std::string p = bundlePath;
    while( p.size() > 1 && p.back() == '/' )
        p.pop_back();

    const std::size_t slash = p.find_last_of( '/' );
    std::string leaf = ( slash == std::string::npos ) ? p : p.substr( slash + 1 );

    const std::size_t dot = leaf.find_last_of( '.' );
    if( dot != std::string::npos )
        leaf = leaf.substr( 0, dot );

    return p + "/Contents/MacOS/" + leaf;
}
#endif

}  // namespace

twClapModule::~twClapModule()
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

bool twClapModule::load( const std::string &path )
{
    path_ = path;

#if defined( _WIN32 )
    const std::wstring wide = widen( path );
    if( wide.empty() ) {
        TW_LOGE( "plugins", "[clap] cannot widen module path '%s'", path.c_str() );
        return false;
    }
    // LOAD_WITH_ALTERED_SEARCH_PATH makes the module's own directory the first
    // stop for its dependent DLLs, which is how plugin vendors ship helper libs.
    // It requires an absolute path (the caller's contract).
    //
    // SEM_FAILCRITICALERRORS for the duration of the load: a truncated, wrong-
    // architecture or otherwise malformed DLL makes the loader raise a MODAL
    // "Ungültiges Bild / Bad Image" message box (status 0xc000012f), which no
    // scan may ever do — one bad file in a plugin directory would otherwise
    // block the scan on a dialog nobody is watching (in the probe process,
    // until the registry's timeout kills it). Per-THREAD, not per-process, so a
    // concurrent scan cannot disturb the host app's own error-mode state.
    DWORD prevErrMode = 0;
    const BOOL errModeSet = SetThreadErrorMode(
        SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &prevErrMode );

    HMODULE h = LoadLibraryExW( wide.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH );

    if( errModeSet )
        SetThreadErrorMode( prevErrMode, nullptr );

    if( !h ) {
        TW_LOGE( "plugins", "[clap] LoadLibraryExW failed for '%s' (error %lu)",
                 path.c_str(), (unsigned long)GetLastError() );
        return false;
    }
    handle_ = (void *)h;
    entry_  = (const clap_plugin_entry_t *)GetProcAddress( h, "clap_entry" );
#else
#if defined( __APPLE__ )
    const std::string dsoPath = macBundleBinary( path );
#else
    const std::string dsoPath = path;
#endif
    void *h = dlopen( dsoPath.c_str(), RTLD_LOCAL | RTLD_NOW );
    if( !h ) {
        const char *why = dlerror();
        TW_LOGE( "plugins", "[clap] dlopen failed for '%s': %s",
                 dsoPath.c_str(), why ? why : "(unknown)" );
        return false;
    }
    handle_ = h;
    entry_  = (const clap_plugin_entry_t *)dlsym( h, "clap_entry" );
#endif

    if( !entry_ ) {
        TW_LOGE( "plugins", "[clap] '%s' exports no clap_entry symbol", path.c_str() );
        return false;
    }
    if( !clap_version_is_compatible( entry_->clap_version ) ) {
        TW_LOGE( "plugins", "[clap] '%s' declares incompatible CLAP version %u.%u.%u",
                 path.c_str(), entry_->clap_version.major, entry_->clap_version.minor,
                 entry_->clap_version.revision );
        entry_ = nullptr;
        return false;
    }
    if( !entry_->init || !entry_->deinit || !entry_->get_factory ) {
        TW_LOGE( "plugins", "[clap] '%s' has an incomplete clap_entry", path.c_str() );
        entry_ = nullptr;
        return false;
    }

    // init() takes the DSO path on Windows/Linux and the BUNDLE path on macOS,
    // i.e. exactly what the caller handed us.
    if( !entry_->init( path.c_str() ) ) {
        TW_LOGE( "plugins", "[clap] clap_entry.init() returned false for '%s'", path.c_str() );
        entry_ = nullptr;  // deinit() must NOT be called after a failed init()
        return false;
    }
    inited_ = true;

    factory_ = (const clap_plugin_factory_t *)entry_->get_factory( CLAP_PLUGIN_FACTORY_ID );
    if( !factory_ || !factory_->get_plugin_count || !factory_->get_plugin_descriptor ||
        !factory_->create_plugin ) {
        TW_LOGE( "plugins", "[clap] '%s' provides no usable clap.plugin-factory", path.c_str() );
        factory_ = nullptr;
        return false;
    }

    TW_LOGI( "plugins", "[clap] loaded '%s' (CLAP %u.%u.%u, %u plugin(s))", path.c_str(),
             entry_->clap_version.major, entry_->clap_version.minor,
             entry_->clap_version.revision,
             (unsigned)factory_->get_plugin_count( factory_ ) );
    return true;
}

void twClapModule::unload()
{
    if( inited_ && entry_ && entry_->deinit )
        entry_->deinit();
    inited_  = false;
    entry_   = nullptr;
    factory_ = nullptr;

    if( handle_ ) {
#if defined( _WIN32 )
        FreeLibrary( (HMODULE)handle_ );
#else
        dlclose( handle_ );
#endif
        handle_ = nullptr;
    }
}

std::shared_ptr<twClapModule> twClapModule::open( const std::string &path )
{
    if( path.empty() )
        return nullptr;

    // make_shared is not usable: the constructor is private.
    std::shared_ptr<twClapModule> mod;
    bool                          loaded = false;

    {
        std::lock_guard<std::mutex> lock( moduleMutex() );

        auto it = moduleTable().find( path );
        if( it != moduleTable().end() ) {
            if( std::shared_ptr<twClapModule> live = it->second.lock() )
                return live;   // a COPY leaves the scope; nothing is destroyed
            moduleTable().erase( it );
        }

        mod.reset( new twClapModule() );
        loaded = mod->load( path );
        if( loaded )
            moduleTable()[path] = mod;
    }

    // A FAILED load must release `mod` with the table mutex RELEASED:
    // ~twClapModule takes that same NON-RECURSIVE mutex to un-intern itself, so
    // destroying the half-built module inside the critical section deadlocks the
    // caller against itself. Only the failure path can reach it, which is why it
    // stayed invisible until the M2 scanner met its first corrupt module — the
    // first code in the repo that deliberately loads files that are not plugins.
    if( !loaded )
        mod.reset();
    return mod;
}

std::vector<twPluginDescriptor> clapModuleDescriptors( const std::string &path )
{
    std::vector<twPluginDescriptor> out;

    std::shared_ptr<twClapModule> mod = twClapModule::open( path );
    if( !mod )
        return out;

    const clap_plugin_factory_t *f = mod->factory();
    const std::uint32_t          n = f->get_plugin_count( f );
    for( std::uint32_t i = 0; i < n; ++i ) {
        const clap_plugin_descriptor_t *cd = f->get_plugin_descriptor( f, i );
        if( !cd || !cd->id )
            continue;

        twPluginDescriptor d;
        d.format = "clap";
        d.uid    = cd->id;
        d.path   = path;
        d.name   = cd->name   ? cd->name   : cd->id;
        d.vendor = cd->vendor ? cd->vendor : "";

        // "instrument" in the feature list is CLAP's own category marker.
        if( cd->features ) {
            for( const char *const *ft = cd->features; *ft; ++ft ) {
                if( std::string( *ft ) == CLAP_PLUGIN_FEATURE_INSTRUMENT ) {
                    d.isInstrument = true;
                    break;
                }
            }
        }

        // The channel counts need a live instance (clap.audio-ports is a plugin
        // extension, not a descriptor field). Creating one is the only honest
        // answer; it is also what makes an out-of-process probe worthwhile in M2.
        d.io = twPluginIoLayout{ 0, 0 };
        if( std::unique_ptr<twPlugin> inst = createClapPlugin( path, d.uid ) )
            d.io = inst->ioLayout();

        out.push_back( std::move( d ) );
    }

    return out;
}

}  // namespace audio
