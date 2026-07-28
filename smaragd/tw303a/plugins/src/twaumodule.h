#ifndef _TWAUMODULE_H_
#define _TWAUMODULE_H_

// AudioUnit backend: discovery + descriptor enumeration (proposal 08 M8).
//
// PRIVATE header of the AU backend, deliberately NOT under include/tw/plugins/:
// the AudioToolbox / CoreFoundation includes and the TW_HAVE_AU define are both
// PRIVATE to tw_plugins (see tw303a/CMakeLists.txt), exactly like the CLAP
// backend's twclapmodule.h. No AudioUnit type ever appears in this header, so it
// is safe to include from twpluginregistry.cc and the out-of-process probe —
// both of which know nothing about CoreAudio.
//
// AU differs from CLAP in one structural way: a plugin's identity is the
// (type, subtype, manufacturer) triple registered with the OS, NOT a dylib at a
// filesystem path. So AU is discovered from the OS component registry
// (AudioComponentFindNext) rather than by walking directories, and a "module" is
// ONE component. To reuse the path-keyed scan cache / sticky-failure records /
// out-of-process probe unchanged, each component is given a stable synthetic
// module key of the form "au:<type>-<subtype>-<manufacturer>" (all hex). The
// descriptor's own `uid` is the same triple WITHOUT the "au:" prefix (the format
// field already says "au"); its `path` is left empty — an AU project re-resolves
// by uid, which is machine-independent, so AU projects are more portable than
// path-based ones.

#include "tw/plugins/twplugin.h"
#include "tw/plugins/twplugindescriptor.h"
#include "tw/plugins/twpluginsearchpaths.h"

#include <cstdint>
#include <string>
#include <vector>

namespace audio {

// Enumerate every hostable AudioUnit the OS registry knows about, as scan module
// files (one per component, path = "au:<triple>"). macOS only; the registry
// merges this into the module list it feeds the normal scan loop. Effect and
// music-effect components only — pure instruments (0 audio in) are out of scope
// (gated on the MIDI/note model), same as CLAP note-ports.
std::vector<twPluginModuleFile> enumerateAuModules();

// Descriptors for ONE component, addressed by its "au:<triple>" module key (the
// bundle path spelling is also accepted). Reads channel I/O by instantiating the
// unit — the instantiation the out-of-process probe isolates. Empty on failure.
std::vector<twPluginDescriptor> auModuleDescriptors( const std::string &moduleKey );

// Instantiate one AudioUnit as a twPlugin. `uid` is the "<type>-<subtype>-
// <manufacturer>" triple (hex); `path` is ignored (cosmetic). Referenced by name
// from twPluginRegistry::instantiate — no static-init self-registration.
std::unique_ptr<twPlugin> createAuPlugin( const std::string &path,
                                          const std::string &uid );

// uid <-> component-code helpers (OSType is a uint32). Defined in twaumodule.cc,
// used by both AU .cc files; uint32 signatures keep AudioUnit types out of this
// header so registry/probe can include it. `codesFromUid` accepts an optional
// leading "au:" and tolerates the descriptor-uid spelling.
std::string auUidFromCodes( std::uint32_t type, std::uint32_t subtype,
                            std::uint32_t manufacturer );
bool auCodesFromUid( const std::string &uid, std::uint32_t &type,
                     std::uint32_t &subtype, std::uint32_t &manufacturer );

}  // namespace audio

#endif
