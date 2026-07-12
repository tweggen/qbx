#ifndef _TWPLUGINDESCRIPTOR_H_
#define _TWPLUGINDESCRIPTOR_H_

#include "tw/plugins/twplugin.h"
#include <memory>
#include <string>
#include <vector>

namespace audio {

struct twPluginDescriptor {
    std::string   format;             // "tw" (in-house) | "clap" | "vst3" | "au" | "lv2"
    std::string   uid;                // format-stable unique id
    std::string   path;               // module file (empty for linked-in plugins)
    std::string   name, vendor;
    twPluginIoLayout io;
    bool          isInstrument = false;
};

class twPluginRegistry {
public:
    // Rescan for plugins (off the audio thread).
    void rescan();

    // Get all discovered plugins.
    const std::vector<twPluginDescriptor> &plugins() const;

    // Instantiate a plugin from its descriptor.
    std::unique_ptr<twPlugin> instantiate( const twPluginDescriptor & );

private:
    std::vector<twPluginDescriptor> plugins_;
};

// Factory to get or create the singleton registry.
twPluginRegistry &pluginRegistry();

}  // namespace audio

#endif
