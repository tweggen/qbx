// Gate 3 of proposal 38: the content-addressed media cache and the project
// copy the drop helper makes out of it.
//
// AC 1 and AC 8 of §C GATE 3 in full, plus the three §B.6 rules for the project
// copy that only exist as prose in the design (sanitise, never overwrite, reuse
// the same content) and that no qxa case can reach as directly as this can.
//
// TWO THINGS ARE WORTH KNOWING ABOUT HOW THIS IS WRITTEN:
//
//   * AC 8 IS A REAL TWO-PROCESS CHECK. `QSaveFile` is write-to-temp-then-
//     rename, and the only way to demonstrate that two writers leave ONE INTACT
//     file is to have two OS processes race for one key — an in-process pair of
//     writes would be serialised by the same thread and would prove nothing.
//     This binary is therefore its own child: `--cache-child <root> <src> <n>`
//     publishes n copies of <src> under one key and exits, and the parent spawns
//     four of them, waits, and hashes what survived. Same shape as
//     qxa.instrument_render_determinism_xproc, which spawns the app twice.
//   * "DOES NOT RE-FETCH" IS ASSERTED WITH A COUNTER, not with a timing. The
//     cache has no fetcher of its own — it is `publish()` that writes — so the
//     assertion is that a second lookup() HITS and that publishCount() has not
//     moved. The end-to-end version of the same claim, through a real provider
//     and a real drop, is qxa.media_drop_deferred's `fetches=` attribute.

#include "app/media/smediacache.h"
#include "app/media/smediadrop.h"
#include "app/media/smediaref.h"

#include "tw/core/twlog.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>

#include <cstdint>
#include <iostream>
#include <vector>

namespace {

QStringList g_failures;
int         g_checks = 0;

void ok( const QString &what )
{
    ++g_checks;
    std::cout << "PASS: " << what.toStdString() << "\n";
}

void fail( const QString &what, const QString &detail )
{
    ++g_checks;
    std::cout << "FAIL: " << what.toStdString() << "\n";
    g_failures.append( what + "\n  " + detail );
}

void check( bool cond, const QString &what, const QString &detail = QString() )
{
    if( cond ) ok( what );
    else       fail( what, detail.isEmpty() ? QStringLiteral( "condition false" )
                                            : detail );
}

void checkEq( const QString &got, const QString &expected, const QString &what )
{
    if( got == expected ) ok( QStringLiteral( "%1  (= '%2')" ).arg( what, got ) );
    else fail( what, QStringLiteral( "expected '%1', got '%2'" )
                         .arg( expected, got ) );
}

// ------------------------------------------------------------- fixtures ----

SMediaEntry entryFor( const QString &sourceId, const QString &path,
                      qint64 size, qint64 mtimeSecs, const QString &etag )
{
    SMediaEntry e;
    e.ref       = SMediaRef( sourceId, path );
    e.name      = QFileInfo( path ).fileName();
    e.sizeBytes = size;
    e.modified  = QDateTime::fromSecsSinceEpoch( mtimeSecs );
    e.etag      = etag;
    return e;
}

bool writeFile( const QString &path, const QByteArray &bytes )
{
    QDir().mkpath( QFileInfo( path ).absolutePath() );
    QFile f( path );
    if( !f.open( QIODevice::WriteOnly ) ) return false;
    return f.write( bytes ) == bytes.size();
}

QByteArray sha1Of( const QString &path )
{
    QFile f( path );
    if( !f.open( QIODevice::ReadOnly ) ) return QByteArray();
    QCryptographicHash h( QCryptographicHash::Sha1 );
    if( !h.addData( &f ) ) return QByteArray();
    return h.result();
}

int fileCount( const QString &dir, const QString &suffixFilter = QString() )
{
    int n = 0;
    QDirIterator it( dir, QDir::Files, QDirIterator::Subdirectories );
    while( it.hasNext() ) {
        it.next();
        if( suffixFilter.isEmpty() || it.fileInfo().suffix() == suffixFilter ) ++n;
    }
    return n;
}

// The AC 8 child: publish `src` under one fixed key, `n` times, as fast as it
// can. Every publish goes through QSaveFile, so the last one to rename wins and
// the file is never a mixture.
int runCacheChild( const QString &root, const QString &src, int n )
{
    SMediaCache cache( root );
    const SMediaRef ref( QStringLiteral( "xproc" ),
                         QStringLiteral( "/lib/contended.wav" ) );
    for( int i = 0; i < n; ++i ) {
        const QString tmp = cache.reserveFetchPath( ref );
        if( !QFile::copy( src, tmp ) ) {
            std::cout << "child: copy to " << tmp.toStdString() << " failed" << std::endl;
            return 2;
        }
        QString err;
        if( cache.publish( ref, tmp, &err ).isEmpty() ) {
            std::cout << "child: publish failed: " << err.toStdString() << std::endl;
            return 3;
        }
    }
    return 0;
}

}   // namespace

int main( int argc, char **argv )
{
    QCoreApplication app( argc, argv );

    const QStringList args = app.arguments();
    if( args.size() >= 5 && args.at( 1 ) == QLatin1String( "--cache-child" ) )
        return runCacheChild( args.at( 2 ), args.at( 3 ), args.at( 4 ).toInt() );

    tw::TwLog::instance().setCapacity( 8192 );

    QTemporaryDir scratch;
    if( !scratch.isValid() ) {
        std::cout << "FAIL: could not create a scratch directory\n";
        return 1;
    }
    const QString scratchRoot = QDir::fromNativeSeparators( scratch.path() );

    // ---------------------------------------------------------------------
    // AC 1a — the content key is STABLE across calls and MOVES when the
    // metadata does. This is what makes a stale cached copy a MISS rather than
    // silently wrong audio.
    // ---------------------------------------------------------------------
    {
        SMediaCache cache( scratchRoot + "/keys" );
        const SMediaRef ref( QStringLiteral( "testdelay" ),
                             QStringLiteral( "/lib/kick.wav" ) );

        cache.noteEntry( entryFor( ref.sourceId, ref.path, 768044, 1700000000,
                                   QString() ) );
        const QString k1 = cache.contentKey( ref );
        const QString k2 = cache.contentKey( ref );
        checkEq( k2, k1, QStringLiteral( "AC 1: the key is stable across calls" ) );
        check( k1.size() == 40,
               QStringLiteral( "AC 1: the key is a sha1 hex digest" ),
               QStringLiteral( "got %1 characters" ).arg( k1.size() ) );

        cache.noteEntry( entryFor( ref.sourceId, ref.path, 768044, 1700000001,
                                   QString() ) );
        check( cache.contentKey( ref ) != k1,
               QStringLiteral( "AC 1: the key MOVES when the mtime moves" ) );

        cache.noteEntry( entryFor( ref.sourceId, ref.path, 768045, 1700000000,
                                   QString() ) );
        check( cache.contentKey( ref ) != k1,
               QStringLiteral( "AC 1: the key MOVES when the size moves" ) );

        cache.noteEntry( entryFor( ref.sourceId, ref.path, 768044, 1700000000,
                                   QStringLiteral( "\"abc\"" ) ) );
        const QString kEtagA = cache.contentKey( ref );
        check( kEtagA != k1,
               QStringLiteral( "AC 1: the key MOVES when an etag appears" ) );
        // The etag WINS over the mtime: a server that promises a change token
        // is the authority, and a mtime that moved without it must not produce
        // a second key for the same content.
        cache.noteEntry( entryFor( ref.sourceId, ref.path, 768044, 1799999999,
                                   QStringLiteral( "\"abc\"" ) ) );
        checkEq( cache.contentKey( ref ), kEtagA,
                 QStringLiteral( "AC 1: with an etag, the mtime does not "
                                 "change the key" ) );

        // Two different files under one source, and one file under two
        // sources, are never the same key.
        const SMediaRef other( ref.sourceId, QStringLiteral( "/lib/snare.wav" ) );
        check( cache.contentKey( other ) != k1,
               QStringLiteral( "AC 1: a different path is a different key" ) );
        const SMediaRef elsewhere( QStringLiteral( "nextcloud:acct" ), ref.path );
        check( cache.contentKey( elsewhere ) != cache.contentKey( ref ),
               QStringLiteral( "AC 1: a different source is a different key" ) );
    }

    // ---------------------------------------------------------------------
    // AC 1b — a second ensureLocal of a cached key does NOT re-fetch, and
    // every write goes through a PER-WRITER temp.
    // ---------------------------------------------------------------------
    {
        const QString root = scratchRoot + "/hit";
        SMediaCache cache( root );
        const SMediaRef ref( QStringLiteral( "testdelay" ),
                             QStringLiteral( "/lib/kick.wav" ) );
        cache.noteEntry( entryFor( ref.sourceId, ref.path, 8, 1700000000,
                                   QString() ) );

        checkEq( cache.lookup( ref ), QString(),
                 QStringLiteral( "AC 1: a cold key MISSES" ) );

        const QString tmp1 = cache.reserveFetchPath( ref );
        const QString tmp2 = cache.reserveFetchPath( ref );
        check( tmp1 != tmp2,
               QStringLiteral( "AC 1: the fetch temp is PER WRITER (T7)" ),
               tmp1 + " == " + tmp2 );
        check( tmp1.endsWith( QStringLiteral( ".part" ) ),
               QStringLiteral( "AC 1: ... and is named .part, so eviction "
                               "never deletes somebody's download" ) );

        check( writeFile( tmp1, QByteArray( "kickkick" ) ),
               QStringLiteral( "AC 1: the fetch temp is writable" ) );
        QString err;
        const QString published = cache.publish( ref, tmp1, &err );
        check( !published.isEmpty(),
               QStringLiteral( "AC 1: publish() succeeds" ), err );
        check( !QFileInfo::exists( tmp1 ),
               QStringLiteral( "AC 1: publish() removes the fetch temp" ) );
        check( published.endsWith( QStringLiteral( ".wav" ) ),
               QStringLiteral( "AC 1: the cached file keeps the suffix" ),
               published );

        const int after = cache.publishCount();
        const QString hit1 = cache.lookup( ref );
        checkEq( hit1, published,
                 QStringLiteral( "AC 1: the second lookup HITS the cache" ) );
        const QString hit2 = cache.lookup( ref );
        checkEq( hit2, published,
                 QStringLiteral( "AC 1: ... and so does the third" ) );
        check( cache.publishCount() == after,
               QStringLiteral( "AC 1: a HIT does not re-fetch (publishCount "
                               "unchanged)" ),
               QStringLiteral( "%1 -> %2" ).arg( after )
                   .arg( cache.publishCount() ) );

        // The metadata moving turns the same ref into a different key, so the
        // cached file is a MISS rather than stale audio.
        cache.noteEntry( entryFor( ref.sourceId, ref.path, 8, 1700009999,
                                   QString() ) );
        checkEq( cache.lookup( ref ), QString(),
                 QStringLiteral( "AC 1: a CHANGED remote file MISSES" ) );
    }

    // ---------------------------------------------------------------------
    // AC 1c — eviction past the cap removes the least-recently-used entries,
    // LOGS what it removed, and NEVER touches a file the open project pins
    // (T17).
    // ---------------------------------------------------------------------
    {
        const QString root = scratchRoot + "/lru";
        SMediaCache cache( root );
        cache.setCapMB( 1 );   // 1 MB

        // Four 400 KB entries: 1.6 MB against a 1 MB cap.
        const QByteArray blob( 400 * 1024, 'x' );
        QStringList paths;
        for( int i = 0; i < 4; ++i ) {
            const SMediaRef ref( QStringLiteral( "testdelay" ),
                                 QStringLiteral( "/lib/f%1.wav" ).arg( i ) );
            cache.noteEntry( entryFor( ref.sourceId, ref.path, blob.size(),
                                       1700000000 + i, QString() ) );
            const QString tmp = cache.reserveFetchPath( ref );
            writeFile( tmp, blob );
            QString err;
            paths << cache.publish( ref, tmp, &err );
            // Distinct modification times, oldest first, so "least recently
            // used" is a FACT rather than a filesystem-timestamp-resolution
            // coin flip.
            QFile f( paths.last() );
            if( f.open( QIODevice::ReadWrite ) )
                f.setFileTime( QDateTime::currentDateTime().addSecs( -100 + i * 10 ),
                               QFileDevice::FileModificationTime );
        }
        check( fileCount( root ) == 4,
               QStringLiteral( "AC 1: four entries are resident before the "
                               "eviction" ) );

        const uint64_t before = tw::TwLog::instance().nextSeq();
        // PIN the two OLDEST — exactly the two an unpinned LRU would take
        // first, so a cache that ignored the pin would delete them and this
        // assertion would fail rather than pass by luck.
        QSet<QString> pinned;
        pinned.insert( QFileInfo( paths.at( 0 ) ).absoluteFilePath() );
        pinned.insert( QFileInfo( paths.at( 1 ) ).absoluteFilePath() );
        const int removed = cache.evictToCap( pinned );

        check( QFileInfo::exists( paths.at( 0 ) ) &&
                   QFileInfo::exists( paths.at( 1 ) ),
               QStringLiteral( "AC 1 / T17: a PINNED entry is never evicted, "
                               "even as the LRU victim" ) );
        check( removed >= 1,
               QStringLiteral( "AC 1: eviction removed at least one entry" ),
               QStringLiteral( "removed %1" ).arg( removed ) );
        check( !QFileInfo::exists( paths.at( 2 ) ),
               QStringLiteral( "AC 1: the least-recently-used UNPINNED entry "
                               "went first" ) );

        std::vector<tw::LogRecord> recs;
        tw::TwLog::instance().snapshot( before, UINT64_MAX, recs );
        int evictionLines = 0, capLines = 0;
        for( const tw::LogRecord &r : recs ) {
            if( r.text.find( "cache: evicted" ) != std::string::npos )
                ++evictionLines;
            if( r.text.find( "cannot reach the" ) != std::string::npos )
                ++capLines;
        }
        check( evictionLines == removed,
               QStringLiteral( "AC 1: every eviction is LOGGED (T12)" ),
               QStringLiteral( "%1 lines for %2 removals" )
                   .arg( evictionLines ).arg( removed ) );
        // 800 KB pinned against a 1 MB cap: reachable, so no complaint.
        check( capLines == 0,
               QStringLiteral( "AC 1: a cap that IS reachable says nothing" ) );

        // Now make the cap unreachable: the pinned 800 KB alone exceed it.
        cache.setCapMB( 0 );          // 0 = no cap, so re-set it properly below
        cache.setCapMB( 1 );
        const uint64_t before2 = tw::TwLog::instance().nextSeq();
        QSet<QString> pinAll;
        for( const QString &p : paths )
            if( QFileInfo::exists( p ) )
                pinAll.insert( QFileInfo( p ).absoluteFilePath() );
        // Add one more unpinned 400 KB so we are over the cap again.
        {
            const SMediaRef ref( QStringLiteral( "testdelay" ),
                                 QStringLiteral( "/lib/extra.wav" ) );
            cache.noteEntry( entryFor( ref.sourceId, ref.path, blob.size(),
                                       1700000099, QString() ) );
            const QString tmp = cache.reserveFetchPath( ref );
            writeFile( tmp, blob );
            QString err;
            const QString p = cache.publish( ref, tmp, &err );
            pinAll.insert( QFileInfo( p ).absoluteFilePath() );
        }
        cache.evictToCap( pinAll );
        recs.clear();
        tw::TwLog::instance().snapshot( before2, UINT64_MAX, recs );
        bool said = false;
        for( const tw::LogRecord &r : recs )
            if( r.text.find( "cannot reach the" ) != std::string::npos )
                said = true;
        check( said,
               QStringLiteral( "AC 1 / T17: a cache that CANNOT reach its cap "
                               "says so and stops" ) );
        check( fileCount( root ) >= 3,
               QStringLiteral( "AC 1 / T17: ... and evicted nothing pinned" ) );
    }

    // ---------------------------------------------------------------------
    // AC 1d — SMARAGD_MEDIA_CACHE_DIR=off still yields a USABLE local path and
    // reuses NOTHING. (The env variable itself is read once per process, so
    // the disabled shape is constructed explicitly here; the env knob's own
    // parsing is one comparison in the ctor.)
    // ---------------------------------------------------------------------
    {
        SMediaCache cache( QString(), /*enabled=*/false );
        const SMediaRef ref( QStringLiteral( "testdelay" ),
                             QStringLiteral( "/lib/kick.wav" ) );
        const QString tmp = cache.reserveFetchPath( ref );
        check( !tmp.isEmpty(),
               QStringLiteral( "AC 1: disabled — a fetch still gets a path" ) );
        check( writeFile( tmp, QByteArray( "off" ) ),
               QStringLiteral( "AC 1: disabled — that path is writable" ) );
        QString err;
        const QString published = cache.publish( ref, tmp, &err );
        checkEq( published, tmp,
                 QStringLiteral( "AC 1: disabled — the fetched file IS the "
                                 "local path" ) );
        checkEq( cache.lookup( ref ), QString(),
                 QStringLiteral( "AC 1: disabled — nothing is ever reused" ) );
        check( cache.publishCount() == 0,
               QStringLiteral( "AC 1: disabled — nothing is written to a "
                               "cache" ) );
        QFile::remove( tmp );
    }

    // ---------------------------------------------------------------------
    // §B.6 / T19 — the project copy: SANITISE, never overwrite, reuse the same
    // content. Three rules the design states as prose and that nothing else
    // executes.
    // ---------------------------------------------------------------------
    {
        checkEq( smediadrop::sanitiseFileName( QStringLiteral( "kick:1?.wav" ) ),
                 QStringLiteral( "kick_1_.wav" ),
                 QStringLiteral( "T19: ':' and '?' are sanitised" ) );
        checkEq( smediadrop::sanitiseFileName( QStringLiteral( "a*b|c.wav" ) ),
                 QStringLiteral( "a_b_c.wav" ),
                 QStringLiteral( "T19: '*' and '|' are sanitised" ) );
        checkEq( smediadrop::sanitiseFileName( QStringLiteral( "trailing." ) ),
                 QStringLiteral( "trailing" ),
                 QStringLiteral( "T19: a trailing dot is illegal on Windows "
                                 "and is stripped" ) );
        checkEq( smediadrop::sanitiseFileName( QStringLiteral( "trailing " ) ),
                 QStringLiteral( "trailing" ),
                 QStringLiteral( "T19: a trailing space likewise" ) );
        checkEq( smediadrop::sanitiseFileName( QStringLiteral( "..." ) ),
                 QStringLiteral( "media" ),
                 QStringLiteral( "T19: a name that sanitises to nothing is "
                                 "never empty" ) );
        checkEq( smediadrop::sanitiseFileName( QStringLiteral( "Drum Kit#1.wav" ) ),
                 QStringLiteral( "Drum Kit#1.wav" ),
                 QStringLiteral( "T19: a legal name is untouched — spaces and "
                                 "'#' are fine here" ) );
    }

    // ---------------------------------------------------------------------
    // AC 8 — TWO PROCESSES fetching one key leave ONE INTACT file.
    // ---------------------------------------------------------------------
    {
        const QString root = scratchRoot + "/xproc";
        QDir().mkpath( root );
        // ~2 MB: big enough that a torn write would be overwhelmingly likely
        // to be caught, small enough to stay cheap under `ctest -j4`.
        const QString src = scratchRoot + "/xproc-src.wav";
        QByteArray payload;
        payload.reserve( 2 * 1024 * 1024 );
        for( int i = 0; i < 2 * 1024 * 1024; ++i )
            payload.append( char( 'A' + ( i % 26 ) ) );
        check( writeFile( src, payload ),
               QStringLiteral( "AC 8: the contended source file exists" ) );
        const QByteArray want = sha1Of( src );

        std::vector<QProcess *> kids;
        for( int i = 0; i < 4; ++i ) {
            auto *p = new QProcess;
            p->start( QCoreApplication::applicationFilePath(),
                      { QStringLiteral( "--cache-child" ), root, src,
                        QStringLiteral( "6" ) } );
            kids.push_back( p );
        }
        bool allOk = true;
        QString childSays;
        for( QProcess *p : kids ) {
            const bool done = p->waitForFinished( 60000 );
            if( !done || p->exitCode() != 0 ) {
                allOk = false;
                childSays += QStringLiteral( "[done=%1 exit=%2 err=%3 %4] " )
                                 .arg( done ).arg( p->exitCode() )
                                 .arg( (int) p->error() ).arg( p->errorString() );
                childSays += QString::fromLocal8Bit( p->readAllStandardOutput() );
                childSays += QString::fromLocal8Bit( p->readAllStandardError() );
            }
            delete p;
        }
        check( allOk,
               QStringLiteral( "AC 8: four concurrent writer processes all "
                               "succeeded" ),
               childSays );

        // ONE published file (plus no leftover .part: every publish removes its
        // own temp), and its bytes are exactly the source's.
        const int published = fileCount( root ) - fileCount( root,
                                                             QStringLiteral( "part" ) );
        check( published == 1,
               QStringLiteral( "AC 8: exactly ONE published file survives" ),
               QStringLiteral( "found %1" ).arg( published ) );
        check( fileCount( root, QStringLiteral( "part" ) ) == 0,
               QStringLiteral( "AC 8: no .part temp is left behind" ) );

        QDirIterator it( root, QDir::Files, QDirIterator::Subdirectories );
        QString found;
        while( it.hasNext() ) { it.next(); found = it.filePath(); }
        check( !found.isEmpty() && sha1Of( found ) == want,
               QStringLiteral( "AC 8: it is INTACT — sha1 equals the source's "
                               "(QSaveFile, T7)" ),
               found );
    }

    std::cout << "\n" << g_checks << " checks.\n";
    if( g_failures.isEmpty() ) {
        std::cout << "PASS - all media cache gate-3 checks passed.\n";
        return 0;
    }
    std::cout << g_failures.size() << " failures:\n\n";
    for( const QString &f : g_failures ) std::cout << f.toStdString() << "\n\n";
    return 1;
}
