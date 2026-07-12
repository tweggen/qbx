#include "tw/plugins/twplugindescriptor.h"
#include "tw/plugins/twplugin.h"
#include <memory>

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
    // Future: scan CLAP directories, load descriptors from cache, etc.
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
    return nullptr;
}

}  // namespace audio
