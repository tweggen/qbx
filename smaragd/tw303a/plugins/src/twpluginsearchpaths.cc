#include "tw/plugins/twpluginsearchpaths.h"

#include "tw/core/twlog.h"

#include <QChar>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <algorithm>
#include <set>

namespace audio {

namespace {

// The separator the platform uses inside CLAP_PATH / VST3_PATH.
#ifdef Q_OS_WIN
constexpr char16_t kEnvSep = u';';
#else
constexpr char16_t kEnvSep = u':';
#endif

void addDir( std::vector<std::string> &out, const QString &d )
{
    if( d.trimmed().isEmpty() ) return;
    const QString clean = QDir::cleanPath( QDir::fromNativeSeparators( d.trimmed() ) );
    if( clean.isEmpty() ) return;
    for( const std::string &s : out )
        if( QString::compare( QString::fromStdString( s ), clean,
#ifdef Q_OS_WIN
                              Qt::CaseInsensitive
#else
                              Qt::CaseSensitive
#endif
                              ) == 0 )
            return;
    out.push_back( clean.toStdString() );
}

void addEnvList( std::vector<std::string> &out, const char *var )
{
    const QString v = qEnvironmentVariable( var );
    if( v.isEmpty() ) return;
    for( const QString &part : v.split( QChar( kEnvSep ), Qt::SkipEmptyParts ) )
        addDir( out, part );
}

// "*.clap" -> "clap". Only formats we can actually probe are reported; a .vst3
// found before M6 lands would otherwise be probed, fail, and be cached as a
// permanent failure that M6 then has to force-clear.
const char *formatForFile( const QString &name )
{
    if( name.endsWith( ".clap", Qt::CaseInsensitive ) ) return "clap";
    return nullptr;
}

// A macOS-style bundle: <Foo.clap>/Contents/MacOS/Foo. Returns an empty string
// when there is no such inner binary (a plain-file module, or a Windows folder
// that merely happens to be named *.clap).
QString bundleBinary( const QFileInfo &fi )
{
    const QString base = fi.completeBaseName();     // "Foo" out of "Foo.clap"
    const QString inner = fi.absoluteFilePath() + "/Contents/MacOS/" + base;
    return QFileInfo::exists( inner ) ? inner : QString();
}

void walk( const QString &dir, int depth, std::set<QString> &seenDirs,
           std::vector<twPluginModuleFile> &out )
{
    // Depth is bounded rather than unbounded-with-cycle-detection because a
    // user can point a search path at their home directory; 8 is deeper than
    // any real vendor layout and cheap to refuse beyond.
    if( depth > 8 ) return;

    const QString canon = QFileInfo( dir ).canonicalFilePath();
    if( canon.isEmpty() ) return;                   // gone, or a broken link
    if( !seenDirs.insert( canon ).second ) return;  // symlink loop

    const QFileInfoList entries =
        QDir( dir ).entryInfoList( QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot );
    for( const QFileInfo &fi : entries ) {
        const char *fmt = formatForFile( fi.fileName() );
        if( fmt ) {
            twPluginModuleFile m;
            m.path   = fi.absoluteFilePath().toStdString();
            m.format = fmt;

            QFileInfo stat = fi;
            if( fi.isDir() ) {
                const QString inner = bundleBinary( fi );
                if( inner.isEmpty() ) {
                    // Named like a module but neither a file nor a bundle:
                    // descend instead of reporting something unloadable.
                    walk( fi.absoluteFilePath(), depth + 1, seenDirs, out );
                    continue;
                }
                stat = QFileInfo( inner );
            }
            m.sizeBytes = (std::uint64_t) std::max<qint64>( 0, stat.size() );
            m.mtimeMs   = (std::int64_t) stat.lastModified().toMSecsSinceEpoch();
            out.push_back( std::move( m ) );
            continue;                               // never descend into a module
        }
        if( fi.isDir() )
            walk( fi.absoluteFilePath(), depth + 1, seenDirs, out );
    }
}

}  // namespace

std::vector<std::string> twPluginSearchPaths::defaults( const std::string &format )
{
    std::vector<std::string> out;

    // The upper-case folder name each format uses under the standard roots.
    QString folder;
    const char *envVar = nullptr;
    if( format == "clap" ) {
        folder = "CLAP";
        envVar = "CLAP_PATH";
    } else if( format == "vst3" ) {
        folder = "VST3";
        envVar = "VST3_PATH";
    } else {
        TW_LOGW( "plugins", "[searchpaths] no default locations known for format '%s'",
                 format.c_str() );
        return out;
    }

#ifdef Q_OS_WIN
    // CLAP and VST3 both specify %CommonProgramFiles%\<FMT> as the system
    // location. CommonProgramW6432 is the 64-bit one seen from a 32-bit process;
    // harmless (and empty) in a 64-bit one, so both are offered.
    for( const char *v : { "CommonProgramFiles", "CommonProgramW6432" } ) {
        const QString root = qEnvironmentVariable( v );
        if( !root.isEmpty() ) addDir( out, root + "/" + folder );
    }
    const QString localApp = qEnvironmentVariable( "LOCALAPPDATA" );
    if( !localApp.isEmpty() )
        addDir( out, localApp + "/Programs/Common/" + folder );
#elif defined( Q_OS_MAC )
    addDir( out, "/Library/Audio/Plug-Ins/" + folder );
    addDir( out, QDir::homePath() + "/Library/Audio/Plug-Ins/" + folder );
#else
    // The CLAP spec's Linux locations; VST3 uses the same shape.
    addDir( out, QDir::homePath() + "/." + folder.toLower() );
    addDir( out, "/usr/lib/" + folder.toLower() );
    addDir( out, "/usr/local/lib/" + folder.toLower() );
#endif

    addEnvList( out, envVar );
    return out;
}

std::vector<twPluginModuleFile> enumeratePluginModules(
    const std::vector<std::string> &dirs )
{
    std::vector<twPluginModuleFile> out;
    std::set<QString>               seenDirs;

    for( const std::string &d : dirs ) {
        const QString qd = QString::fromStdString( d );
        if( qd.trimmed().isEmpty() ) continue;
        const QFileInfo fi( qd );
        if( !fi.exists() ) continue;                // not installed: not an error
        if( fi.isDir() ) walk( qd, 0, seenDirs, out );
    }

    // Deterministic order, de-duplicated: two search paths may overlap (a
    // parent and a child), and the cache key is the path, so a duplicate would
    // be probed twice and reported twice.
    std::sort( out.begin(), out.end(),
               []( const twPluginModuleFile &a, const twPluginModuleFile &b ) {
                   return a.path < b.path;
               } );
    out.erase( std::unique( out.begin(), out.end(),
                            []( const twPluginModuleFile &a, const twPluginModuleFile &b ) {
                                return a.path == b.path;
                            } ),
               out.end() );
    return out;
}

}  // namespace audio
