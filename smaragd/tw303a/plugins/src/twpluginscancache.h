#ifndef _TWPLUGINSCANCACHE_H_
#define _TWPLUGINSCANCACHE_H_

// The frame the out-of-process probe wraps its JSON result in, and the host
// unwraps. Shared here because plugin_probe.cc and twpluginregistry.cc are the
// only two files that may ever know these strings, and they MUST agree.
//
// It exists because the probe process has a third-party plugin loaded into it
// and a plugin may write to stdout -- which is the channel the result travels
// on. See the comment at the emit site in plugin_probe.cc for the measured
// case (an iPlug2 VST3 whose banner turned a good plugin into a sticky FAILED
// cache record).
#define TW_PROBE_JSON_BEGIN "---TW-PROBE-JSON-BEGIN---"
#define TW_PROBE_JSON_END   "---TW-PROBE-JSON-END---"

// PRIVATE header of the plugin scanner (proposal 08 M2).
//
// The scan cache is <configDir>/plugincache.v<kScannerVersion>.json -- the name
// carries the version because the config dir is shared by every build this user
// runs, and a single file meant two builds invalidated each other's records on
// every launch. See twPluginRegistry::cacheFileName(). QJson deliberately: tw_core
// already links Qt Core, the scan path is not realtime, and a human-readable
// table is worth a lot when a user reports "it does not find my plugin".
//
// It is NOT twSidecarStore. That store keys on a content hash of audio PCM and
// caps itself with an LRU, which would silently evict the plugin table — the
// exact failure mode of "the second launch is slow again, sometimes".
//
// Kept out of plugins/include/ because it names Qt JSON types and is an
// implementation detail; the probe executable reaches it through
// plugins/src being on its include path.

#include "tw/plugins/twplugindescriptor.h"

#include <QJsonObject>

#include <map>
#include <string>

namespace audio {

// One descriptor as a JSON object. Shared by the cache and the out-of-process
// probe so the two can never disagree about the field names.
QJsonObject descriptorToJson( const twPluginDescriptor &d );
bool        descriptorFromJson( const QJsonObject &o, twPluginDescriptor &d );

// Read/write the whole table, keyed by module path. A file that is missing,
// unparseable, or written by a different scannerVersion yields an EMPTY map
// (everything is re-probed) rather than a partial one — a half-trusted cache is
// worse than a cold one. Returns false when nothing usable was loaded.
bool loadPluginScanCache( const std::string &path, int scannerVersion,
                          std::map<std::string, twPluginModuleRecord> &out );

// Written through a temporary + rename so a crash mid-write cannot leave a
// truncated table behind. Returns false (and logs) on any I/O failure; the scan
// result itself is unaffected, the next launch is just cold.
bool savePluginScanCache( const std::string &path, int scannerVersion,
                          const std::map<std::string, twPluginModuleRecord> &in );

}  // namespace audio

#endif
