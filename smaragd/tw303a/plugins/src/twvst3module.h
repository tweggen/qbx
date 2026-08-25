#ifndef _TWVST3MODULE_H_
#define _TWVST3MODULE_H_

// PRIVATE header of the VST3 backend (proposal 08 M6).
//
// Same rule as twclapmodule.h: it is deliberately NOT under
// plugins/include/tw/plugins/, because the pluginterfaces include directory and
// TW_HAVE_VST3 are PRIVATE to tw_plugins. A public header that changed shape
// with TW_HAVE_VST3 would give every other module a different view of the same
// types (ODR/ABI skew) — CONTRACT invariant 4.
//
// Only compiled when the vst3_pluginterfaces submodule is present, so there is
// no #ifdef inside.

#include "tw/plugins/twplugin.h"
#include "tw/plugins/twplugindescriptor.h"

#include "pluginterfaces/base/ipluginbase.h"

#include <memory>
#include <string>
#include <vector>

namespace audio {

// One loaded VST3 module (DSO or bundle).
//
// The module entry point (InitDll / bundleEntry / ModuleEntry) must be called
// once per DSO and matched by its exit, exactly like CLAP's init/deinit — so
// modules are interned by absolute path and shared: open() returns the live
// instance if one exists, and the DSO is unloaded when the last shared_ptr
// (instance or scanner) goes away.
class twVst3Module {
public:
    ~twVst3Module();

    // Load, or join, the module at path. Returns nullptr on failure (the reason
    // is logged through TW_LOG*). Never throws. Not for the audio thread.
    //
    // `path` is the .vst3 the SCANNER reported — a bundle directory or, on
    // Windows, a plain DLL. Resolving the binary inside a bundle is this
    // class's job, and the interning key stays the path the caller gave.
    static std::shared_ptr<twVst3Module> open( const std::string &path );

    Steinberg::IPluginFactory *factory() const { return factory_; }
    const std::string         &path()    const { return path_; }

    twVst3Module( const twVst3Module & )            = delete;
    twVst3Module &operator=( const twVst3Module & ) = delete;

private:
    twVst3Module() = default;

    bool load( const std::string &path );
    void unload();

    std::string                path_;
    void                      *handle_   = nullptr;  // HMODULE / dlopen handle
    // macOS only: the CFBundleRef the .vst3 bundle was opened as, retained for
    // this module's lifetime and released in unload(). Held as void* so that
    // CoreFoundation stays out of this header — nothing but the .cc needs it.
    // Null for a FLAT module (a dylib renamed .vst3, which is what the in-repo
    // twtestvst3 fixture is): there is no bundle to name, and bundleEntry's
    // argument is then legitimately null.
    void                      *cfBundle_ = nullptr;
    Steinberg::IPluginFactory *factory_  = nullptr;
    const char                *exitName_ = nullptr;
    bool                       inited_   = false;
};

// A VST3 class id is 16 opaque bytes; the engine's descriptor uid is a string.
// The canonical spelling here is 32 upper-case hex digits IN COM (Windows
// GUID) BYTE ORDER, ALWAYS — regardless of which platform produced the TUID
// (plugins/CONTRACT.md; the .cc file explains the COM_COMPATIBLE mechanism
// this is canonicalizing across). That is what makes it safe in an XML
// attribute: a project saved on Windows and one saved on Linux/macOS name the
// SAME plugin with the SAME string. Both directions, because the loader has
// to get back to the TUID it was given.
std::string vst3UidFromTuid( const Steinberg::TUID uid );
bool        vst3TuidFromUid( const std::string &uid, Steinberg::TUID out );

// Enumerate the audio-effect classes a VST3 module offers, as engine descriptors
// (format "vst3", uid = the class id in hex, path = the module the scanner saw).
// Empty on failure. The module is loaded and released around the call, so this is
// the cheap scan-time query — the registry's probe and plugin_probe.cc are its
// callers.
std::vector<twPluginDescriptor> vst3ModuleDescriptors( const std::string &path );

// Instantiate one plugin out of a module. uid is the hex class id; empty means
// "the first audio-effect class in the factory". Returns nullptr (logged) on
// failure. Referenced by name from twPluginRegistry::instantiate() — discovery
// stays symbol-referenced, never static-init self-registration (CONTRACT
// invariant 1).
std::unique_ptr<twPlugin> createVst3Plugin( const std::string &path,
                                            const std::string &uid );

}  // namespace audio

#endif
