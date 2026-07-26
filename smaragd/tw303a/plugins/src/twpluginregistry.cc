#include "tw/plugins/twplugindescriptor.h"
#include "tw/plugins/twplugin.h"
#include "tw/core/twlog.h"
#include <memory>

#ifdef TW_HAVE_CLAP
// PRIVATE header of the CLAP backend. The TW_HAVE_CLAP define and the clap
// include directory are both PRIVATE to tw_plugins (see tw303a/CMakeLists.txt),
// so this #ifdef can only ever appear in a .cc inside this module — never in a
// public tw/plugins/*.h, where it would give other modules a different view of
// the same declarations.
#include "twclapmodule.h"
#endif

namespace audio {

// Forward declare the PassThrough plugin factory.
std::unique_ptr<twPlugin> createPassThroughPlugin();

// Static registry instance.
static twPluginRegistry gRegistry;

twPluginRegistry &pluginRegistry()
{
    return gRegistry;
}

void twPluginRegistry::rescan()
{
    // For now, hardcode the PassThrough plugin as the only available plugin.
    // The CLAP *backend* landed in proposal 08 M1 (instantiate() below knows the
    // format); the scanner that populates this list from disk — search paths,
    // mtime cache, out-of-process probe — is M2.
    plugins_.clear();

    twPluginDescriptor passThrough;
    passThrough.format = "tw";
    passThrough.uid = "tw.passthrough";
    passThrough.path = "";  // linked-in, not a separate module
    passThrough.name = "PassThrough";
    passThrough.vendor = "Smaragd";
    passThrough.io = {2, 2};
    passThrough.isInstrument = false;

    plugins_.push_back( passThrough );
}

const std::vector<twPluginDescriptor> &twPluginRegistry::plugins() const
{
    // Lazy initialization: ensure plugins are loaded on first access
    if( plugins_.empty() ) {
        const_cast<twPluginRegistry*>(this)->rescan();
    }
    return plugins_;
}

std::unique_ptr<twPlugin> twPluginRegistry::instantiate( const twPluginDescriptor &desc )
{
    if( desc.uid == "tw.passthrough" ) {
        return createPassThroughPlugin();
    }

    if( desc.format == "clap" ) {
#ifdef TW_HAVE_CLAP
        // Symbol-referenced discovery (CONTRACT invariant 1): the registry names
        // the backend factory directly, so nothing depends on static-init
        // self-registration surviving static-library linking.
        return createClapPlugin( desc.path, desc.uid );
#else
        TW_LOGE( "plugins", "[registry] cannot instantiate CLAP plugin '%s': this build "
                 "has no CLAP support (the third_party/clap submodule was missing at "
                 "configure time)", desc.uid.c_str() );
        return nullptr;
#endif
    }

    TW_LOGW( "plugins", "[registry] no backend for plugin format '%s' (uid '%s')",
             desc.format.c_str(), desc.uid.c_str() );
    return nullptr;
}

}  // namespace audio
