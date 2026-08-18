#include "app/media/smediacache.h"

#include "tw/core/twlog.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QVector>

#include <algorithm>

namespace {

// The knob (§B.6), read ONCE per process for the same reason
// TW_STRETCH_BACKEND is: a value that could change mid-run would make two
// halves of one case disagree about where the cache is.
QString envCacheDir()
{
    static const QString v =
        QString::fromLocal8Bit( qgetenv( "SMARAGD_MEDIA_CACHE_DIR" ) ).trimmed();
    return v;
}

qint64 dirBytes( const QString &root )
{
    qint64 total = 0;
    QDirIterator it( root, QDir::Files, QDirIterator::Subdirectories );
    while( it.hasNext() ) { it.next(); total += it.fileInfo().size(); }
    return total;
}

}   // namespace

SMediaCache::SMediaCache()
{
    const QString env = envCacheDir();
    if( env.compare( QStringLiteral( "off" ), Qt::CaseInsensitive ) == 0 ) {
        enabled_    = false;
        rootLocked_ = true;
        TW_LOGI( "media", "cache: DISABLED by SMARAGD_MEDIA_CACHE_DIR=off" );
        return;
    }
    if( !env.isEmpty() ) {
        root_       = QDir::fromNativeSeparators( env );
        rootLocked_ = true;
        TW_LOGI( "media", "cache: root from SMARAGD_MEDIA_CACHE_DIR: %s",
                 root_.toUtf8().constData() );
        return;
    }
    // No knob and (yet) no injection. A sane standalone default so a media
    // cache constructed before the shell has run is never silently rootless;
    // SApplication overwrites this with the real config dir, which is the
    // authority (app_core cannot see SSettings — that is a LAYER boundary, not
    // a style choice).
    root_ = QDir( QStandardPaths::writableLocation(
                      QStandardPaths::AppConfigLocation ) )
                .filePath( QStringLiteral( "mediacache" ) );
}

SMediaCache::SMediaCache( const QString &root, bool enabled, QObject *parent )
    : QObject( parent ),
      root_( QDir::fromNativeSeparators( root ) ),
      enabled_( enabled ),
      rootLocked_( true )
{
}

SMediaCache &SMediaCache::instance()
{
    static SMediaCache c;
    return c;
}

void SMediaCache::setRoot( const QString &dir )
{
    if( rootLocked_ ) {
        TW_LOGD( "media", "cache: setRoot('%s') ignored — "
                          "SMARAGD_MEDIA_CACHE_DIR owns the root",
                 dir.toUtf8().constData() );
        return;
    }
    root_ = QDir::fromNativeSeparators( dir );
    TW_LOGI( "media", "cache: root %s", root_.toUtf8().constData() );
}

void SMediaCache::setCapMB( qint64 mb )
{
    capMB_ = mb;
}

void SMediaCache::ensureRoot() const
{
    if( root_.isEmpty() ) return;
    QDir().mkpath( root_ );
}

QString SMediaCache::bucketFor( const QString &sourceId ) const
{
    // A source id may contain a colon ("nextcloud:<accountId>") and anything
    // else a deployment cares to put in it, so it is HASHED rather than used as
    // a directory name. §B.6's "<sourceId-hash>".
    const QByteArray h = QCryptographicHash::hash( sourceId.toUtf8(),
                                                   QCryptographicHash::Sha1 );
    return QString::fromLatin1( h.toHex().left( 16 ) );
}

QString SMediaCache::suffixFor( const SMediaRef &ref ) const
{
    const QString suffix = QFileInfo( ref.path ).suffix().toLower();
    // Only a plain alphanumeric suffix is carried through; anything else would
    // be putting a remote server's spelling into a local file name.
    for( QChar c : suffix )
        if( !c.isLetterOrNumber() ) return QString();
    return suffix.left( 8 );
}

void SMediaCache::noteEntry( const SMediaEntry &entry )
{
    if( entry.isDir || !entry.ref.isValid() ) return;
    Meta m;
    m.etag      = entry.etag;
    m.size      = entry.sizeBytes;
    m.mtimeSecs = entry.modified.isValid() ? entry.modified.toSecsSinceEpoch()
                                           : -1;
    known_.insert( entry.ref.toUri(), m );
}

QString SMediaCache::contentKey( const SMediaRef &ref ) const
{
    QCryptographicHash h( QCryptographicHash::Sha1 );
    h.addData( ref.sourceId.toUtf8() );
    h.addData( "\n", 1 );
    h.addData( ref.path.toUtf8() );
    h.addData( "\n", 1 );
    const auto it = known_.constFind( ref.toUri() );
    if( it != known_.constEnd() ) {
        // etag WINS over mtime where a server offers one: it is the only
        // change token a WebDAV server promises to move on every write.
        const QByteArray token =
            it->etag.isEmpty()
                ? QByteArray::number( it->mtimeSecs )
                : it->etag.toUtf8();
        h.addData( token );
        h.addData( "\n", 1 );
        h.addData( QByteArray::number( it->size ) );
    } else {
        // Never listed here. The key still exists (an un-listed drop must not
        // FAIL) but it cannot detect a remote change, so it is marked as such
        // rather than colliding with a metadata-bearing key for the same file.
        h.addData( "-\n-", 3 );
    }
    return QString::fromLatin1( h.result().toHex() );
}

QString SMediaCache::lookup( const SMediaRef &ref )
{
    if( !enabled_ || root_.isEmpty() || !ref.isValid() ) return QString();
    const QString suffix = suffixFor( ref );
    QString path = QDir( root_ ).filePath( bucketFor( ref.sourceId ) + '/' +
                                           contentKey( ref ) );
    if( !suffix.isEmpty() ) path += '.' + suffix;
    if( !QFileInfo::exists( path ) ) return QString();

    // A hit TOUCHES the file. The LRU order IS the modification time: it needs
    // no index file, and therefore no cross-process index to tear.
    // ReadWrite, not ReadOnly: on Windows a handle opened GENERIC_READ has no
    // FILE_WRITE_ATTRIBUTES and SetFileTime fails on it. A failure here costs
    // only LRU precision, so it is not an error — but silently never touching
    // anything would make the LRU an insertion order, so it is worth getting
    // right.
    QFile f( path );
    if( f.open( QIODevice::ReadWrite ) )
        f.setFileTime( QDateTime::currentDateTime(),
                       QFileDevice::FileModificationTime );
    return path;
}

QString SMediaCache::reserveFetchPath( const SMediaRef &ref ) const
{
    // PER-WRITER, always (T7). Two processes fetching one key each write their
    // own .part; publish() then makes exactly one of them the published file.
    const QString name =
        QStringLiteral( "%1.%2.%3.part" )
            .arg( contentKey( ref ) )
            .arg( QCoreApplication::applicationPid() )
            .arg( ++fetchSeq_ );
    if( !enabled_ || root_.isEmpty() )
        return QDir::temp().filePath( QStringLiteral( "smaragd-media-" ) + name );
    const QString bucket = QDir( root_ ).filePath( bucketFor( ref.sourceId ) );
    QDir().mkpath( bucket );
    return QDir( bucket ).filePath( name );
}

QString SMediaCache::publish( const SMediaRef &ref, const QString &fetched,
                              QString *error )
{
    if( error ) error->clear();
    if( !QFileInfo::exists( fetched ) ) {
        if( error ) *error = QStringLiteral( "the fetched file is not there" );
        return QString();
    }
    if( !enabled_ || root_.isEmpty() ) {
        // Disabled: the per-request temp IS the local path, and nothing is
        // reused. Stated, never silent.
        TW_LOGD( "media", "cache: disabled — %s stays where it was fetched",
                 ref.path.toUtf8().constData() );
        return fetched;
    }

    ensureRoot();
    const QString bucket = QDir( root_ ).filePath( bucketFor( ref.sourceId ) );
    QDir().mkpath( bucket );
    const QString suffix = suffixFor( ref );
    QString target = QDir( bucket ).filePath( contentKey( ref ) );
    if( !suffix.isEmpty() ) target += '.' + suffix;

    // ALREADY THERE? A content-addressed key means equal key implies equal
    // content, so another writer having got there first is a HIT, not a
    // conflict. Checked before the copy so the common concurrent case costs
    // nothing.
    if( QFileInfo::exists( target ) ) {
        QFile::remove( fetched );
        return target;
    }

    QFile in( fetched );
    if( !in.open( QIODevice::ReadOnly ) ) {
        if( error ) *error = in.errorString();
        return QString();
    }

    // QSaveFile: a per-writer temp plus an atomic rename, done by Qt. This is
    // the ONE thing standing between `ctest -j4` and the sidecar store's torn
    // payload (T7) — do not "simplify" it to a QFile::rename or a copy.
    QSaveFile out( target );
    if( !out.open( QIODevice::WriteOnly ) ) {
        if( error ) *error = out.errorString();
        return QString();
    }
    char buf[ 64 * 1024 ];
    for( ;; ) {
        const qint64 n = in.read( buf, sizeof( buf ) );
        if( n < 0 ) { if( error ) *error = in.errorString(); return QString(); }
        if( n == 0 ) break;
        if( out.write( buf, n ) != n ) {
            if( error ) *error = out.errorString();
            return QString();
        }
    }
    in.close();
    if( !out.commit() ) {
        // A LOST RENAME IS NOT A FAILURE. On Windows two processes committing
        // the same target can collide in MoveFileEx, and the loser's answer is
        // the winner's file — which is byte-identical by construction, because
        // the key IS the content. This is what makes AC 8 ("two processes
        // fetching one key leave ONE INTACT file") a property of the design
        // rather than of who won.
        if( QFileInfo::exists( target ) ) {
            TW_LOGD( "media", "cache: another writer published %s first",
                     QFileInfo( target ).fileName().toUtf8().constData() );
            QFile::remove( fetched );
            return target;
        }
        if( error ) *error = out.errorString();
        return QString();
    }
    QFile::remove( fetched );
    ++publishCount_;
    TW_LOGD( "media", "cache: published %s (%lld bytes)",
             QFileInfo( target ).fileName().toUtf8().constData(),
             (long long) QFileInfo( target ).size() );
    return target;
}

bool SMediaCache::clearAll()
{
    if( !rootLocked_ || root_.isEmpty() ) {
        TW_LOGW( "media", "cache: clearAll() REFUSED — the root was not named "
                          "by SMARAGD_MEDIA_CACHE_DIR" );
        return false;
    }
    QDir dir( root_ );
    if( dir.exists() && !dir.removeRecursively() ) {
        TW_LOGW( "media", "cache: clearAll() could not remove %s",
                 root_.toUtf8().constData() );
        return false;
    }
    QDir().mkpath( root_ );
    known_.clear();
    publishCount_ = 0;
    TW_LOGI( "media", "cache: cleared %s", root_.toUtf8().constData() );
    return true;
}

qint64 SMediaCache::totalBytes() const
{
    if( !enabled_ || root_.isEmpty() ) return 0;
    return dirBytes( root_ );
}

int SMediaCache::evictToCap( const QSet<QString> &pinned )
{
    if( !enabled_ || root_.isEmpty() || capMB_ <= 0 ) return 0;
    const qint64 cap = capMB_ * 1024LL * 1024LL;

    struct Ent { QString path; qint64 size; qint64 mtime; };
    QVector<Ent> ents;
    qint64 total = 0;
    QDirIterator it( root_, QDir::Files, QDirIterator::Subdirectories );
    while( it.hasNext() ) {
        it.next();
        const QFileInfo fi = it.fileInfo();
        // A .part is another writer's in-flight fetch. Never evictable: it is
        // not published yet and deleting it is deleting somebody's download.
        if( fi.suffix() == QLatin1String( "part" ) ) continue;
        ents.push_back( { fi.absoluteFilePath(), fi.size(),
                          fi.lastModified().toMSecsSinceEpoch() } );
        total += fi.size();
    }
    if( total <= cap ) return 0;

    std::sort( ents.begin(), ents.end(),
               []( const Ent &a, const Ent &b ) { return a.mtime < b.mtime; } );

    int removed = 0;
    qint64 pinnedBytes = 0;
    for( const Ent &e : ents ) {
        if( total <= cap ) break;
        // THE PIN (T17). The cap is a cache policy; the project is not a cache.
        if( pinned.contains( e.path ) ) { pinnedBytes += e.size; continue; }
        if( !QFile::remove( e.path ) ) {
            TW_LOGW( "media", "cache: could not evict %s",
                     e.path.toUtf8().constData() );
            continue;
        }
        total -= e.size;
        ++removed;
        // Every eviction is LOGGED (§B.6, T12): a cache that silently deleted
        // audio would announce itself as a clip that is suddenly silent.
        TW_LOGI( "media", "cache: evicted %s (%lld bytes), %lld left of %lld",
                 QFileInfo( e.path ).fileName().toUtf8().constData(),
                 (long long) e.size, (long long) total, (long long) cap );
    }
    if( total > cap ) {
        // Stated and STOPPED, never evicted anyway.
        TW_LOGW( "media", "cache: cannot reach the %lld MB cap — %lld bytes "
                          "resident, %lld of them PINNED by the open project. "
                          "Nothing further will be evicted.",
                 (long long) capMB_, (long long) total,
                 (long long) pinnedBytes );
    }
    return removed;
}
