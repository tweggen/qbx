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

#include "twclapmodule.h"
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

int main( int argc, char **argv )
{
    if( argc != 2 ) {
        std::fprintf( stderr, "usage: %s <module-path>\n",
                      argc > 0 ? argv[0] : "smaragd_pluginprobe" );
        return 2;
    }

    const std::string path = argv[1];
    const QString     qpath = QString::fromLocal8Bit( path.c_str() );

    std::vector<audio::twPluginDescriptor> descs;
    if( qpath.endsWith( ".clap", Qt::CaseInsensitive ) ) {
        descs = audio::clapModuleDescriptors( path );
    } else {
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
    std::fwrite( json.constData(), 1, (std::size_t) json.size(), stdout );
    std::fputc( '\n', stdout );
    std::fflush( stdout );
    return 0;
}
