
#include <QDir>
#include <QFileInfo>
#include <QStringList>

#include "app/model/sfilepathref.h"

namespace {

// Path comparison follows the FILESYSTEM, not the locale: Windows and macOS
// hand out case-insensitive volumes by default, so "C:/Users" and "c:/users"
// name the same directory and must compare equal — otherwise a perfectly
// project-relative sample would be written out as an absolute path just
// because the two paths disagreed on capitalisation.
Qt::CaseSensitivity pathCase()
{
#if defined( Q_OS_WIN ) || defined( Q_OS_MAC )
    return Qt::CaseInsensitive;
#else
    return Qt::CaseSensitive;
#endif
}

// An absolute, cleaned path with '/' separators (what QFileInfo hands out on
// every platform, Windows included).
QString cleanAbs( const QString &p )
{
    if( p.isEmpty() ) return QString();
    return QDir::cleanPath( QFileInfo( p ).absoluteFilePath() );
}

QStringList segments( const QString &cleanAbsPath )
{
    // "/home/u/x" -> ["", "home", "u", "x"];  "C:/u/x" -> ["C:", "u", "x"]
    return cleanAbsPath.split( QLatin1Char( '/' ) );
}

QString pathFromSegments( const QStringList &s )
{
    if( s.isEmpty() ) return QString();
    const QString joined = s.join( QLatin1Char( '/' ) );
    if( joined.isEmpty() ) return QStringLiteral( "/" );          // POSIX root
    if( s.size() == 1 && joined.endsWith( QLatin1Char( ':' ) ) )
        return joined + QLatin1Char( '/' );                       // "C:" -> "C:/"
    return joined;
}

int commonPrefixLength( const QStringList &a, const QStringList &b )
{
    const int n = qMin( a.size(), b.size() );
    int i = 0;
    while( i < n && a.at( i ).compare( b.at( i ), pathCase() ) == 0 ) i++;
    return i;
}

bool climbs( const QString &relative )
{
    // Only a WHOLE ".." segment is a climb; a directory named "..stuff" is not.
    const QStringList parts = relative.split( QLatin1Char( '/' ) );
    for( const QString &p : parts ) {
        if( p == QLatin1String( ".." ) ) return true;
    }
    return false;
}

} // namespace

QString SFilePathRef::toStored( const QString &filePath,
                                const QString &projectFilePath )
{
    if( filePath.isEmpty() ) return filePath;

    const QString target = cleanAbs( filePath );
    if( projectFilePath.isEmpty() ) return target;      // untitled: no anchor

    const QString projectDir =
        QDir::cleanPath( QFileInfo( cleanAbs( projectFilePath ) ).absolutePath() );
    if( projectDir.isEmpty() ) return target;

    const QStringList tSegs = segments( target );
    const QStringList pSegs = segments( projectDir );

    // No shared root at all (different Windows drives, or a UNC share versus a
    // local volume): nothing relative can be said. Rule 3.
    const int nCommon = commonPrefixLength( pSegs, tSegs );
    if( nCommon == 0 ) return target;

    const QString relative = QDir( projectDir ).relativeFilePath( target );
    if( relative.isEmpty() ) return target;             // degenerate; be safe

    // Rule 1, the common case: the file sits inside the project's own subtree,
    // so the reference climbs nowhere and is already as portable as it gets.
    if( !climbs( relative ) ) return relative;

    // It climbs. WHERE it stops is what decides between the three rules — the
    // common ancestor is exactly the directory the "../.." chain lands on.
    const QString ancestor = pathFromSegments( pSegs.mid( 0, nCommon ) );
    const QString home     = cleanAbs( QDir::homePath() );

    if( !home.isEmpty() ) {
        const QStringList hSegs = segments( home );
        // Rule 2: it climbs exactly TO home. Re-anchor there.
        if( nCommon == hSegs.size()
            && commonPrefixLength( pSegs.mid( 0, nCommon ), hSegs ) == hSegs.size() ) {
            const QString fromHome = QDir( home ).relativeFilePath( target );
            return fromHome.isEmpty() ? QStringLiteral( "~" )
                                      : QStringLiteral( "~/" ) + fromHome;
        }
        // Rule 3: it climbs PAST home (the ancestor is a proper ancestor of the
        // home directory), so it has left everything the user owns behind.
        if( nCommon < hSegs.size()
            && commonPrefixLength( pSegs.mid( 0, nCommon ), hSegs ) == nCommon ) {
            return target;
        }
    }

    // Rule 3 also covers a climb to a filesystem root outside home (a project
    // living on D:\ reaching for D:\something-else): there is no enclosing
    // folder left that could be moved as a unit, so relative buys nothing.
    if( QDir( ancestor ).isRoot() ) return target;

    // Anything else still climbs within a shared, movable subtree. Rule 1.
    return relative;
}

QString SFilePathRef::fromStored( const QString &storedPath,
                                  const QString &projectFilePath )
{
    if( storedPath.isEmpty() ) return storedPath;

    // Form 2 — the only reserved spelling.
    if( storedPath == QLatin1String( "~" ) ) return cleanAbs( QDir::homePath() );
    if( storedPath.startsWith( QLatin1String( "~/" ) ) ) {
        return QDir::cleanPath( QDir::homePath() + QLatin1Char( '/' )
                                + storedPath.mid( 2 ) );
    }

    // Form 3.
    if( QFileInfo( storedPath ).isAbsolute() ) return QDir::cleanPath( storedPath );

    // Form 1 — needs the anchor. Without one, hand the caller back what it gave
    // us so it can fall back to its own resolution (CWD, script base dir, ...).
    if( projectFilePath.isEmpty() ) return storedPath;

    const QString projectDir =
        QFileInfo( cleanAbs( projectFilePath ) ).absolutePath();
    if( projectDir.isEmpty() ) return storedPath;
    return QDir::cleanPath( QDir( projectDir ).filePath( storedPath ) );
}
