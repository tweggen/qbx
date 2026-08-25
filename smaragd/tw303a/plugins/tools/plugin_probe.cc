// smaragd_pluginprobe — the out-of-process plugin scanner (proposal 08 M2,
// proposal 08 §Decisions 2).
//
//   smaragd_pluginprobe <module-path>
//
// Loads ONE module with the production loader, enumerates what its factory
// offers, and writes the descriptors to stdout as JSON:
//
//   {"path":"...","plugins":[{"format":"clap","uid":"...","name":"...",
//                             "vendor":"...","nIn":2,"nOut":2,
//                             "isInstrument":false}]}
//
// Exit code 0 when at least one descriptor was produced, 1 otherwise, and
// whatever the operating system reports when the plugin takes the process down.
// That last case is the whole reason this program exists: a plugin that
// segfaults or hangs while being instantiated kills the PROBE, and the registry
// turns that into a cached failed/timeout record instead of losing the app.
//
// Under plugins/tools/ because tools/ is an ALLOW_DIR in tools/check_logging.py:
// the JSON on stdout IS this program's output, not a diagnostic. Diagnostics go
// through TW_LOG* to stderr, so they can never corrupt the JSON.

#ifdef TW_HAVE_CLAP
#include "twclapmodule.h"
#endif
#ifdef TW_HAVE_VST3
#include "twvst3module.h"
#endif
#ifdef TW_HAVE_AU
#include "twaumodule.h"
#endif
#include "twpluginscancache.h"

#include "tw/core/twlog.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <cstdio>
#include <string>
#include <vector>

#if defined( _WIN32 )
#include <windows.h>
#endif

namespace {

// This process is EXPECTED to die on bad input — that is its purpose. Windows
// must therefore never answer a bad module with a modal dialog: the loader's
// "Bad Image" box (0xc000012f) for a truncated or wrong-architecture DLL, and
// the Windows Error Reporting box when a plugin crashes inside clap_entry->init.
// Either one turns a cheap failed/timeout cache record into a message box in the
// user's face, plus a probe that hangs until the registry's timeout fires.
// Process-wide is right here: this whole process exists only to load one module.
void silenceOsFaultDialogs()
{
#if defined( _WIN32 )
    SetErrorMode( SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX
                  | SEM_NOOPENFILEERRORBOX );
#endif
}

} // namespace

int main( int argc, char **argv )
{
    silenceOsFaultDialogs();

    if( argc != 2 ) {
        std::fprintf( stderr, "usage: %s <module-path>\n",
                      argc > 0 ? argv[0] : "smaragd_pluginprobe" );
        return 2;
    }

    const std::string path = argv[1];
    const QString     qpath = QString::fromLocal8Bit( path.c_str() );

    std::vector<audio::twPluginDescriptor> descs;
    bool                                   known = false;
#ifdef TW_HAVE_CLAP
    if( !known && qpath.endsWith( ".clap", Qt::CaseInsensitive ) ) {
        descs = audio::clapModuleDescriptors( path );
        known = true;
    }
#endif
#ifdef TW_HAVE_VST3
    if( !known && qpath.endsWith( ".vst3", Qt::CaseInsensitive ) ) {
        descs = audio::vst3ModuleDescriptors( path );
        known = true;
    }
#endif
#ifdef TW_HAVE_AU
    // AU "modules" are the synthetic per-component key "au:<triple>" the registry
    // enumerator produces; the .component spelling is accepted for symmetry.
    if( !known && ( qpath.startsWith( "au:" )
                    || qpath.endsWith( ".component", Qt::CaseInsensitive ) ) ) {
        descs = audio::auModuleDescriptors( path );
        known = true;
    }
#endif
    if( !known ) {
        TW_LOGE( "plugins", "[probe] '%s' is not a format this build can probe",
                 path.c_str() );
        return 1;
    }

    if( descs.empty() ) {
        TW_LOGE( "plugins", "[probe] '%s' yielded no usable descriptor", path.c_str() );
        return 1;
    }

    QJsonArray plugins;
    for( const audio::twPluginDescriptor &d : descs )
        plugins.append( audio::descriptorToJson( d ) );

    QJsonObject root;
    root["path"]    = qpath;
    root["plugins"] = plugins;

    const QByteArray json = QJsonDocument( root ).toJson( QJsonDocument::Compact );

    // THE SENTINEL IS NOT DECORATION. This process has a THIRD-PARTY PLUGIN
    // loaded into it, and a plugin may write to stdout — which is the channel
    // this result travels on. Measured on an iPlug2-based VST3 (Mangrove): the
    // plugin prints a 10-line "BEGIN IPLUG CHANNEL IO PARSER" banner from its
    // module initialiser, so the host's QJsonDocument::fromJson() saw banner +
    // JSON, failed to parse, and recorded a perfectly good plugin as a FAILED
    // module — sticky in plugincache.json, so it stayed missing until someone
    // force-rescanned. The probe exited 0 and its JSON was valid; only the
    // channel was dirty.
    //
    // Redirecting the plugin's stdout is not available to us (it writes to the
    // same fd, and a plugin may legitimately want a console), so the result is
    // FRAMED instead and the host takes what is between the markers. Anything
    // the plugin prints, before or after, is then noise by construction.
    std::fputs( "\n" TW_PROBE_JSON_BEGIN "\n", stdout );
    std::fwrite( json.constData(), 1, (std::size_t) json.size(), stdout );
    std::fputs( "\n" TW_PROBE_JSON_END "\n", stdout );
    std::fflush( stdout );
    return 0;
}
