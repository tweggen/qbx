#ifndef _TWCLAPMODULE_H_
#define _TWCLAPMODULE_H_

// PRIVATE header of the CLAP backend (proposal 08 M1).
//
// It is deliberately NOT under plugins/include/tw/plugins/: only files inside
// tw_plugins may see <clap/clap.h>, because the clap include directory and the
// TW_HAVE_CLAP definition are both PRIVATE to that target. A public header that
// changed shape with TW_HAVE_CLAP would give every other module a different
// view of the same types (ODR/ABI skew).
//
// This whole translation unit pair is only compiled when the CLAP submodule is
// present — see the _tw_clap_found block in tw303a/CMakeLists.txt — so there is
// no #ifdef inside it.

#include "tw/plugins/twplugin.h"
#include "tw/plugins/twplugindescriptor.h"

#include <clap/clap.h>

#include <memory>
#include <string>
#include <vector>

namespace audio {

// One loaded CLAP DSO.
//
// CLAP requires init()/deinit() to be called in matched pairs, once per DSO
// (clap/entry.h). Several plugin instances may come from the same .clap file,
// so modules are interned by their absolute path and shared: open() returns the
// live instance if one exists, and the DSO is unloaded when the last
// shared_ptr — instance or scanner — goes away.
class twClapModule {
public:
    ~twClapModule();

    // Load, or join, the module at path. Returns nullptr on failure (the reason
    // is logged through TW_LOG*). Never throws. Not for the audio thread.
    static std::shared_ptr<twClapModule> open( const std::string &path );

    const clap_plugin_factory_t *factory() const { return factory_; }
    const std::string           &path()    const { return path_; }

    twClapModule( const twClapModule & )            = delete;
    twClapModule &operator=( const twClapModule & ) = delete;

private:
    twClapModule() = default;

    bool load( const std::string &path );
    void unload();

    std::string                  path_;
    void                        *handle_  = nullptr;  // HMODULE / dlopen handle
    const clap_plugin_entry_t   *entry_   = nullptr;
    const clap_plugin_factory_t *factory_ = nullptr;
    bool                         inited_  = false;
};

// Enumerate the plugins a CLAP module offers, as engine descriptors (format
// "clap", uid = the CLAP plugin id, path = the module file). Empty on failure.
// The module is loaded and released around the call, so this is the cheap
// scan-time query — M2's scanner and tools/clap_probe.cc are its callers.
std::vector<twPluginDescriptor> clapModuleDescriptors( const std::string &path );

// Instantiate one plugin out of a module. uid is the CLAP plugin id; empty means
// "the first plugin in the factory". Returns nullptr (logged) on failure.
// Referenced by name from twPluginRegistry::instantiate() — discovery stays
// symbol-referenced, never static-init self-registration (CONTRACT invariant 1).
std::unique_ptr<twPlugin> createClapPlugin( const std::string &path,
                                            const std::string &uid );

}  // namespace audio

#endif
