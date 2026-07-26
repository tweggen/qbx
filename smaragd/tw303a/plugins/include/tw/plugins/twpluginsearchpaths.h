#ifndef _TWPLUGINSEARCHPATHS_H_
#define _TWPLUGINSEARCHPATHS_H_

#include <cstdint>
#include <string>
#include <vector>

namespace audio {

// Where plugins live, per platform and per format (proposal 08 M2).
//
// This is only the DEFAULT: the app persists the user's edited list and pushes
// it into twPluginRegistry::setSearchPaths(). Nothing here reads or writes
// settings, so it stays usable from the headless tests and the probe.
struct twPluginSearchPaths {
    // format is "clap" or "vst3" (lower case). Unknown formats yield an empty
    // list rather than guessing. Directories that do not exist are still
    // returned — a user who installs a plugin later should not have to re-add
    // the standard folder, and the scanner simply skips what is not there.
    // The format's own environment variable (CLAP_PATH / VST3_PATH) is appended,
    // split on the platform's path separator.
    static std::vector<std::string> defaults( const std::string &format );
};

// One module file the scanner found.
struct twPluginModuleFile {
    std::string   path;               // absolute
    std::string   format;             // derived from the extension
    std::uint64_t sizeBytes = 0;
    std::int64_t  mtimeMs   = 0;
};

// Walk `dirs` recursively and return every module file, de-duplicated by
// absolute path and sorted so a scan is deterministic. A bundle (a DIRECTORY
// named *.clap / *.vst3, which is how macOS ships them) is reported as one
// module and not descended into; its size/mtime come from the inner binary when
// that is resolvable, so touching the wrapper does not invalidate the cache.
std::vector<twPluginModuleFile> enumeratePluginModules(
    const std::vector<std::string> &dirs );

}  // namespace audio

#endif
