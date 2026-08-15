#include "twpluginscancache.h"

#include "tw/core/twlog.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QSaveFile>
#include <QString>

namespace audio {

namespace {

const char *statusName( twPluginModuleStatus s )
{
    switch( s ) {
    case twPluginModuleStatus::Ok:      return "ok";
    case twPluginModuleStatus::Failed:  return "failed";
    case twPluginModuleStatus::Timeout: return "timeout";
    }
    return "failed";
}

// Anything we do not recognise is treated as Failed, never as Ok: a record we
// cannot interpret must not put a plugin into the registry.
twPluginModuleStatus statusFromName( const QString &s )
{
    if( s == "ok" )      return twPluginModuleStatus::Ok;
    if( s == "timeout" ) return twPluginModuleStatus::Timeout;
    return twPluginModuleStatus::Failed;
}

}  // namespace

QJsonObject descriptorToJson( const twPluginDescriptor &d )
{
    QJsonObject o;
    o["format"]       = QString::fromStdString( d.format );
    o["uid"]          = QString::fromStdString( d.uid );
    o["path"]         = QString::fromStdString( d.path );
    o["name"]         = QString::fromStdString( d.name );
    o["vendor"]       = QString::fromStdString( d.vendor );
    o["nIn"]          = (int) d.io.audioInputs;
    o["nOut"]         = (int) d.io.audioOutputs;
    o["isInstrument"] = d.isInstrument;
    // Proposal 36 P2 (scanner version 2). Written unconditionally so a record
    // is self-describing; a v1 file can never be read as a v2 one anyway
    // (scannerVersion is part of the cache key, invariant 9).
    o["acceptsNotes"]  = d.acceptsNotes;
    o["emitsNotes"]    = d.emitsNotes;
    o["eventPortsIn"]  = (int) d.eventPortsIn;
    o["eventPortsOut"] = (int) d.eventPortsOut;
    o["nOutBuses"]     = (int) d.nOutBuses;
    QJsonArray buses;
    for( std::uint16_t c : d.outBusChannels )
        buses.append( (int) c );
    o["outBusChannels"] = buses;
    return o;
}

bool descriptorFromJson( const QJsonObject &o, twPluginDescriptor &d )
{
    const QString uid = o.value( "uid" ).toString();
    if( uid.isEmpty() ) return false;             // without a uid it is unusable

    d.format       = o.value( "format" ).toString( "clap" ).toStdString();
    d.uid          = uid.toStdString();
    d.path         = o.value( "path" ).toString().toStdString();
    d.name         = o.value( "name" ).toString( uid ).toStdString();
    d.vendor       = o.value( "vendor" ).toString().toStdString();
    d.io.audioInputs  = (std::uint16_t) o.value( "nIn"  ).toInt( 0 );
    d.io.audioOutputs = (std::uint16_t) o.value( "nOut" ).toInt( 0 );
    d.isInstrument = o.value( "isInstrument" ).toBool( false );

    // Proposal 36 P2. Defaults are the pre-36 answers ("no events, one output
    // bus if it has outputs at all"), so a record written by a probe that
    // predates a field degrades to today's behaviour rather than to nonsense.
    d.acceptsNotes  = o.value( "acceptsNotes" ).toBool( false );
    d.emitsNotes    = o.value( "emitsNotes" ).toBool( false );
    d.eventPortsIn  = (std::uint16_t) o.value( "eventPortsIn" ).toInt( d.acceptsNotes ? 1 : 0 );
    d.eventPortsOut = (std::uint16_t) o.value( "eventPortsOut" ).toInt( d.emitsNotes ? 1 : 0 );
    d.nOutBuses     = (std::uint16_t) o.value( "nOutBuses" )
                          .toInt( d.io.audioOutputs > 0 ? 1 : 0 );
    d.outBusChannels.clear();
    for( const QJsonValue &bv : o.value( "outBusChannels" ).toArray() )
        d.outBusChannels.push_back( (std::uint16_t) bv.toInt( 0 ) );
    if( d.outBusChannels.empty() && d.nOutBuses > 0 )
        d.outBusChannels.assign( d.nOutBuses, d.io.audioOutputs );
    return true;
}

bool loadPluginScanCache( const std::string &path, int scannerVersion,
                          std::map<std::string, twPluginModuleRecord> &out )
{
    out.clear();
    if( path.empty() ) return false;

    QFile f( QString::fromStdString( path ) );
    if( !f.exists() ) return false;
    if( !f.open( QIODevice::ReadOnly ) ) {
        TW_LOGW( "plugins", "[scan] cannot read the plugin cache '%s': %s",
                 path.c_str(), f.errorString().toUtf8().constData() );
        return false;
    }
    const QByteArray raw = f.readAll();
    f.close();

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson( raw, &err );
    if( err.error != QJsonParseError::NoError || !doc.isObject() ) {
        TW_LOGW( "plugins", "[scan] plugin cache '%s' is not valid JSON (%s); "
                 "rescanning from scratch", path.c_str(),
                 err.errorString().toUtf8().constData() );
        return false;
    }

    const QJsonObject root = doc.object();
    const int fileVersion = root.value( "scannerVersion" ).toInt( -1 );
    if( fileVersion != scannerVersion ) {
        TW_LOGI( "plugins", "[scan] plugin cache '%s' was written by scanner "
                 "version %d, this build is %d — rescanning everything",
                 path.c_str(), fileVersion, scannerVersion );
        return false;
    }

    const QJsonArray mods = root.value( "modules" ).toArray();
    for( const QJsonValue &mv : mods ) {
        if( !mv.isObject() ) continue;
        const QJsonObject mo = mv.toObject();

        twPluginModuleRecord rec;
        rec.path = mo.value( "path" ).toString().toStdString();
        if( rec.path.empty() ) continue;
        rec.sizeBytes      = (std::uint64_t) mo.value( "sizeBytes" ).toDouble( 0.0 );
        rec.mtimeMs        = (std::int64_t)  mo.value( "mtimeMs" ).toDouble( 0.0 );
        rec.scannerVersion = mo.value( "scannerVersion" ).toInt( fileVersion );
        rec.status         = statusFromName( mo.value( "status" ).toString() );

        if( rec.status == twPluginModuleStatus::Ok ) {
            for( const QJsonValue &pv : mo.value( "plugins" ).toArray() ) {
                if( !pv.isObject() ) continue;
                twPluginDescriptor d;
                if( descriptorFromJson( pv.toObject(), d ) ) {
                    // The record's path is authoritative: a cache copied to
                    // another machine must not keep instantiating from a stale
                    // absolute path stored inside the descriptor.
                    d.path = rec.path;
                    rec.plugins.push_back( std::move( d ) );
                }
            }
            // "ok with nothing in it" cannot happen from a real scan; refusing
            // it keeps a hand-edited file from hiding a module forever.
            if( rec.plugins.empty() ) continue;
        }

        out[rec.path] = std::move( rec );
    }

    // %u + a cast, not %zu: MinGW's printf format checker uses the msvcrt
    // conversions and rejects the C99 length modifier.
    TW_LOGD( "plugins", "[scan] plugin cache '%s': %u module record(s)",
             path.c_str(), (unsigned) out.size() );
    return !out.empty();
}

bool savePluginScanCache( const std::string &path, int scannerVersion,
                          const std::map<std::string, twPluginModuleRecord> &in )
{
    if( path.empty() ) return false;

    QJsonArray mods;
    for( const auto &kv : in ) {
        const twPluginModuleRecord &rec = kv.second;
        QJsonObject mo;
        mo["path"]           = QString::fromStdString( rec.path );
        mo["sizeBytes"]      = (double) rec.sizeBytes;
        mo["mtimeMs"]        = (double) rec.mtimeMs;
        mo["scannerVersion"] = rec.scannerVersion;
        mo["status"]         = QString::fromLatin1( statusName( rec.status ) );
        if( rec.status == twPluginModuleStatus::Ok ) {
            QJsonArray ps;
            for( const twPluginDescriptor &d : rec.plugins )
                ps.append( descriptorToJson( d ) );
            mo["plugins"] = ps;
        }
        mods.append( mo );
    }

    QJsonObject root;
    root["scannerVersion"] = scannerVersion;
    root["modules"]        = mods;

    const QString qpath = QString::fromStdString( path );
    QDir().mkpath( QFileInfo( qpath ).absolutePath() );

    // QSaveFile is write-to-temp-then-rename: a crash while the table is being
    // written leaves the previous cache intact instead of a truncated one that
    // the next launch would discard wholesale.
    QSaveFile f( qpath );
    if( !f.open( QIODevice::WriteOnly ) ) {
        TW_LOGW( "plugins", "[scan] cannot write the plugin cache '%s': %s",
                 path.c_str(), f.errorString().toUtf8().constData() );
        return false;
    }
    f.write( QJsonDocument( root ).toJson( QJsonDocument::Indented ) );
    if( !f.commit() ) {
        TW_LOGW( "plugins", "[scan] cannot commit the plugin cache '%s': %s",
                 path.c_str(), f.errorString().toUtf8().constData() );
        return false;
    }
    return true;
}

}  // namespace audio
