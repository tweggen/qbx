// Gate 4 of proposal 38: the Nextcloud/WebDAV connector, gated against an
// in-repo stub server.
//
// The nine ACs of §C GATE 4, one section each. GATE 4 IS UNIT-TEST ONLY: no
// account, no UI, no qxa case reaches this code (that is gate 5c) -- this
// binary and its stub are the entire gate.
//
// The stub (main/testkit/src/swebdavstub.h) binds 127.0.0.1:0 -- an
// OS-assigned port, never a fixed one -- which is what keeps this test safe
// under `ctest -j4`.

#include "app/media/slocalmediasource.h"   // kMaxInFlightPropfinds, kMaxSearchEntries
#include "app/media/smediaref.h"
#include "app/media/smediatypes.h"
#include "app/media/swebdavclient.h"
#include "app/media/swebdavmediasource.h"

#include "swebdavstub.h"

#include "tw/core/twlog.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>
#include <QVector>
#include <QtGlobal>

#include <functional>
#include <iostream>

#ifndef SMEDIA_MODULE_DIR
#error "SMEDIA_MODULE_DIR must be supplied by the build (smaragd/main/media)"
#endif
#ifndef SWEBDAV_TESTKIT_DIR
#error "SWEBDAV_TESTKIT_DIR must be supplied by the build (smaragd/main/testkit/src)"
#endif

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

void checkEq( qint64 got, qint64 expected, const QString &what )
{
    if( got == expected ) {
        ok( QStringLiteral( "%1  (= %2)" ).arg( what ).arg( got ) );
    } else {
        fail( what, QStringLiteral( "expected %1, got %2" )
                        .arg( expected ).arg( got ) );
    }
}

void checkEq( const QString &got, const QString &expected, const QString &what )
{
    if( got == expected ) {
        ok( QStringLiteral( "%1  (= '%2')" ).arg( what, got ) );
    } else {
        fail( what, QStringLiteral( "expected '%1', got '%2'" )
                        .arg( expected, got ) );
    }
}

// --------------------------------------------------------------- plumbing --

bool waitFor( const std::function<bool()> &pred, int timeoutMs = 20000 )
{
    QElapsedTimer t;
    t.start();
    while( t.elapsed() < timeoutMs ) {
        if( pred() ) return true;
        QCoreApplication::processEvents( QEventLoop::AllEvents, 5 );
        QThread::msleep( 1 );
    }
    return pred();
}

void pump( int ms )
{
    QElapsedTimer t;
    t.start();
    while( t.elapsed() < ms ) {
        QCoreApplication::processEvents( QEventLoop::AllEvents, 5 );
        QThread::msleep( 1 );
    }
}

QString moduleDir()
{
    return QDir::fromNativeSeparators( QDir::cleanPath( QStringLiteral( SMEDIA_MODULE_DIR ) ) );
}

QString testkitStubDir()
{
    return QDir::fromNativeSeparators(
        QDir::cleanPath( QStringLiteral( SWEBDAV_TESTKIT_DIR ) ) );
}

QStringList sourceFilesUnder( const QString &root )
{
    QStringList out;
    QDir        d( root );
    QVector<QString> stack{ d.absolutePath() };
    while( !stack.isEmpty() ) {
        const QString here = stack.takeLast();
        QDir          hd( here );
        const QFileInfoList infos =
            hd.entryInfoList( QDir::AllEntries | QDir::NoDotAndDotDot, QDir::Name );
        for( const QFileInfo &fi : infos ) {
            if( fi.isDir() ) {
                stack.append( fi.absoluteFilePath() );
            } else if( fi.suffix() == QLatin1String( "h" )
                       || fi.suffix() == QLatin1String( "cpp" )
                       || fi.suffix() == QLatin1String( "cc" ) ) {
                out << fi.absoluteFilePath();
            }
        }
    }
    out.sort();
    return out;
}

QString readAll( const QString &path )
{
    QFile f( path );
    if( !f.open( QIODevice::ReadOnly | QIODevice::Text ) ) return QString();
    return QString::fromUtf8( f.readAll() );
}

// ------------------------------------------------------ client-level recorder

struct ClientRecorder
{
    struct Ok { int id; QVector<SWebDavClient::Entry> entries; };
    struct Fail { int id; int status; QString message; };
    struct Progress { int id; qint64 done; qint64 total; };
    struct Fin { int id; QString localPath; };

    QVector<Ok>       finished;
    QVector<Fail>     failed;
    QVector<Progress> progress;
    QVector<Fin>      getFinished;
    QVector<Fail>     getFailed;

    void attach( SWebDavClient &c )
    {
        QObject::connect( &c, &SWebDavClient::propfindFinished, &c,
                          [this]( int id, const QVector<SWebDavClient::Entry> &e ) {
                              finished.append( { id, e } );
                          } );
        QObject::connect( &c, &SWebDavClient::propfindFailed, &c,
                          [this]( int id, int status, const QString &msg ) {
                              failed.append( { id, status, msg } );
                          } );
        QObject::connect( &c, &SWebDavClient::getProgress, &c,
                          [this]( int id, qint64 done, qint64 total ) {
                              progress.append( { id, done, total } );
                          } );
        QObject::connect( &c, &SWebDavClient::getFinished, &c,
                          [this]( int id, const QString &path ) {
                              getFinished.append( { id, path } );
                          } );
        QObject::connect( &c, &SWebDavClient::getFailed, &c,
                          [this]( int id, int status, const QString &msg ) {
                              getFailed.append( { id, status, msg } );
                          } );
    }
};

const SWebDavClient::Entry *findByHref( const QVector<SWebDavClient::Entry> &v,
                                        const QString &href )
{
    for( const SWebDavClient::Entry &e : v ) {
        if( e.href == href ) return &e;
    }
    return nullptr;
}

// -------------------------------------------------------- source-level recorder

struct SourceRecorder
{
    struct Batch { int id; QVector<SMediaEntry> entries; bool final; int truncated; };
    struct Failure { int id; QString message; };
    struct Progress { int id; qint64 done; qint64 total; };
    struct Fetched { int id; QString localPath; };

    QVector<Batch>    batches;
    QVector<Failure>  failures;
    QVector<Progress> progressList;
    QVector<Fetched>  fetched;

    void clear()
    {
        batches.clear();
        failures.clear();
        progressList.clear();
        fetched.clear();
    }

    void attach( SWebDavMediaSource &src )
    {
        QObject::connect( &src, &SMediaSource::entriesReady, &src,
                          [this]( int id, const QVector<SMediaEntry> &batch, bool final,
                                  int truncated ) {
                              batches.append( { id, batch, final, truncated } );
                          } );
        QObject::connect( &src, &SMediaSource::requestFailed, &src,
                          [this]( int id, const QString &msg ) {
                              failures.append( { id, msg } );
                          } );
        QObject::connect( &src, &SMediaSource::fetchProgress, &src,
                          [this]( int id, qint64 done, qint64 total ) {
                              progressList.append( { id, done, total } );
                          } );
        QObject::connect( &src, &SMediaSource::fetchFinished, &src,
                          [this]( int id, const QString &path ) {
                              fetched.append( { id, path } );
                          } );
    }

    bool sawFinal( int id ) const
    {
        for( const Batch &b : batches ) if( b.id == id && b.final ) return true;
        return false;
    }

    QVector<SMediaEntry> entriesFor( int id ) const
    {
        QVector<SMediaEntry> out;
        for( const Batch &b : batches ) if( b.id == id ) out += b.entries;
        return out;
    }

    int truncatedOnFinal( int id ) const
    {
        for( const Batch &b : batches ) if( b.id == id && b.final ) return b.truncated;
        return -1;
    }

    bool hasFailure( int id ) const
    {
        for( const Failure &f : failures ) if( f.id == id ) return true;
        return false;
    }
};

} // namespace

int main( int argc, char **argv )
{
    QCoreApplication app( argc, argv );

    SWebDavStub stub;
    check( stub.start(), QStringLiteral( "the stub binds 127.0.0.1:0" ) );
    check( stub.port() != 0, QStringLiteral( "the OS assigned a real port" ),
           QString::number( stub.port() ) );

    const QUrl base = stub.baseUrl();
    std::cout << "stub base url: " << base.toString().toStdString() << "\n\n";

    // ================================================================ AC 2
    // PROPFIND parsing, each assertion isolated. Runs the CLIENT directly so
    // a filtering decision made by SWebDavMediaSource cannot mask a parser
    // defect.
    {
        QDateTime kickModified( QDate( 2026, 1, 12 ), QTime( 10, 0, 0 ), Qt::UTC );

        SWebDavStubEntry kick;
        kick.name      = QStringLiteral( "kick#1.wav" );
        kick.sizeBytes = 768044;
        kick.modified  = kickModified;
        kick.etag      = QStringLiteral( "\"abc123\"" );
        kick.content   = QByteArray( 64, 'K' );

        SWebDavStubEntry sparse;   // missing optional props on purpose
        sparse.name = QStringLiteral( "sparse.wav" );
        sparse.content = QByteArray( 8, 'S' );

        stub.setDirectory( QStringLiteral( "Drum Kit" ), { kick, sparse } );

        SWebDavStubEntry drumKitDir;
        drumKitDir.name  = QStringLiteral( "Drum Kit" );
        drumKitDir.isDir = true;
        stub.setDirectory( QString(), { drumKitDir } );

        SWebDavClient   client( base, QString() );
        ClientRecorder  rec;
        rec.attach( client );

        const int rootId = client.propfind( QString() );
        check( waitFor( [&] { return !rec.finished.isEmpty(); } ),
               QStringLiteral( "AC 2: root PROPFIND completes" ) );
        check( !rec.finished.isEmpty() && rec.finished.last().id == rootId,
               QStringLiteral( "AC 2: the result is tagged with its own request id" ) );

        const QVector<SWebDavClient::Entry> rootEntries = rec.finished.last().entries;
        checkEq( rootEntries.size(), 1,
                QStringLiteral( "AC 2: the root's own collection is excluded from its "
                                "own listing (only 'Drum Kit' remains)" ) );
        if( !rootEntries.isEmpty() ) {
            checkEq( rootEntries.first().name, QStringLiteral( "Drum Kit" ),
                    QStringLiteral( "AC 2: display name" ) );
            checkEq( rootEntries.first().href, QStringLiteral( "Drum Kit" ),
                    QStringLiteral( "AC 2: trailing slashes are normalised out of href" ) );
            check( rootEntries.first().isDir,
                   QStringLiteral( "AC 2: a directory is distinguished by "
                                   "<d:resourcetype><d:collection/></d:resourcetype>" ) );
        }

        rec.finished.clear();
        client.propfind( QStringLiteral( "Drum Kit" ) );
        check( waitFor( [&] { return !rec.finished.isEmpty(); } ),
               QStringLiteral( "AC 2: 'Drum Kit' PROPFIND completes" ) );
        const QVector<SWebDavClient::Entry> subEntries =
            rec.finished.isEmpty() ? QVector<SWebDavClient::Entry>() : rec.finished.last().entries;
        checkEq( subEntries.size(), 2,
                QStringLiteral( "AC 2: two files, 'Drum Kit' itself excluded" ) );

        const auto *kickEntry = findByHref( subEntries, QStringLiteral( "Drum Kit/kick#1.wav" ) );
        check( kickEntry != nullptr,
               QStringLiteral( "AC 2: percent-encoded hrefs decoded "
                               "('Drum%20Kit/kick%231.wav' -> 'Drum Kit/kick#1.wav')" ) );
        if( kickEntry ) {
            checkEq( kickEntry->name, QStringLiteral( "kick#1.wav" ),
                    QStringLiteral( "AC 2: display name" ) );
            checkEq( kickEntry->sizeBytes, 768044, QStringLiteral( "AC 2: size" ) );
            check( !kickEntry->isDir, QStringLiteral( "AC 2: a file is not a directory" ) );
            check( kickEntry->modified.isValid(),
                   QStringLiteral( "AC 2: getlastmodified parsed to a valid QDateTime" ) );
            checkEq( kickEntry->modified.toUTC().toString( Qt::ISODate ),
                    kickModified.toUTC().toString( Qt::ISODate ),
                    QStringLiteral( "AC 2: ... and to the CORRECT QDateTime (RFC 1123)" ) );
        }

        const auto *sparseEntry = findByHref( subEntries, QStringLiteral( "Drum Kit/sparse.wav" ) );
        check( sparseEntry != nullptr, QStringLiteral( "AC 2: the sparse entry is present" ) );
        if( sparseEntry ) {
            checkEq( sparseEntry->sizeBytes, -1,
                    QStringLiteral( "AC 2: a missing getcontentlength yields -1, "
                                    "not a dropped entry" ) );
            check( !sparseEntry->modified.isValid(),
                   QStringLiteral( "AC 2: a missing getlastmodified yields an "
                                   "invalid QDateTime, not a dropped entry" ) );
            check( sparseEntry->etag.isEmpty(),
                   QStringLiteral( "AC 2: a missing getetag yields an empty string" ) );
        }

        // Namespace prefix variance: the commonest real-world WebDAV break.
        // Re-run the SAME listing with "D:" and then with no prefix at all;
        // the parsed result must not move.
        for( const QString &prefix : { QStringLiteral( "D" ), QString() } ) {
            stub.setNamespacePrefix( prefix );
            rec.finished.clear();
            client.propfind( QStringLiteral( "Drum Kit" ) );
            check( waitFor( [&] { return !rec.finished.isEmpty(); } ),
                   QStringLiteral( "AC 2: namespace prefix '%1' -- PROPFIND completes" )
                       .arg( prefix.isEmpty() ? QStringLiteral( "(none)" ) : prefix ) );
            const auto *reKick =
                rec.finished.isEmpty()
                    ? nullptr
                    : findByHref( rec.finished.last().entries,
                                 QStringLiteral( "Drum Kit/kick#1.wav" ) );
            check( reKick != nullptr && reKick->sizeBytes == 768044,
                   QStringLiteral( "AC 2: namespace prefix '%1' -- same parsed result" )
                       .arg( prefix.isEmpty() ? QStringLiteral( "(none)" ) : prefix ) );
        }
        stub.setNamespacePrefix( QStringLiteral( "d" ) );   // restore

        rec.finished.clear();
        client.propfind( QStringLiteral( "does-not-exist" ) );
        check( waitFor( [&] { return !rec.failed.isEmpty(); } ),
               QStringLiteral( "AC 2: a PROPFIND on an unknown directory fails cleanly" ) );
    }

    // ================================================================ AC 3
    // Search: BFS, at most kMaxInFlightPropfinds concurrent, streams,
    // truncates and announces it; non-recursive issues exactly one PROPFIND.
    {
        stub.clearDirectories();
        stub.resetCounters();
        stub.setNamespacePrefix( QStringLiteral( "d" ) );

        // A wide tree: 8 subdirectories under root, each with two matching
        // files -- wide enough that the queue outruns the 4-in-flight cap
        // after the root PROPFIND completes.
        QVector<SWebDavStubEntry> rootEntries;
        for( int i = 0; i < 8; ++i ) {
            SWebDavStubEntry d;
            d.name  = QStringLiteral( "d%1" ).arg( i );
            d.isDir = true;
            rootEntries.append( d );

            QVector<SWebDavStubEntry> children;
            for( int f = 0; f < 2; ++f ) {
                SWebDavStubEntry file;
                file.name    = QStringLiteral( "track%1.wav" ).arg( f );
                file.content = QByteArray( 4, 'x' );
                children.append( file );
            }
            SWebDavStubEntry notAudio;
            notAudio.name    = QStringLiteral( "readme.txt" );
            notAudio.content = QByteArray( 4, 'r' );
            children.append( notAudio );
            stub.setDirectory( QStringLiteral( "d%1" ).arg( i ), children );
        }
        stub.setDirectory( QString(), rootEntries );

        // Widen the concurrency window: without a delay, loopback round trips
        // can complete faster than the next PROPFIND is issued and the peak
        // would read 1 by accident rather than by design.
        stub.setResponseDelayMs( 30 );

        SWebDavMediaSource src( QStringLiteral( "acct1" ), base, QString() );
        SourceRecorder      rec;
        rec.attach( src );

        const int searchId =
            src.search( QString(), QString(), /*recursive=*/true, smedia::kAudio );
        check( waitFor( [&] { return rec.sawFinal( searchId ); }, 30000 ),
               QStringLiteral( "AC 3: the recursive search completes" ) );

        checkEq( stub.peakConcurrentRequests(), smedia::kMaxInFlightPropfinds,
                QStringLiteral( "AC 3: at most (and, on this wide a tree, exactly) "
                                "kMaxInFlightPropfinds PROPFINDs are ever concurrently "
                                "in flight" ) );

        const QVector<SMediaEntry> found = rec.entriesFor( searchId );
        checkEq( found.size(), 16,
                QStringLiteral( "AC 3: 8 dirs x 2 audio files, readme.txt excluded by "
                                "the suffix filter" ) );
        checkEq( rec.truncatedOnFinal( searchId ), 0,
                QStringLiteral( "AC 3: a complete search announces no truncation" ) );

        stub.setResponseDelayMs( 0 );

        // Non-recursive: exactly one PROPFIND, the root's.
        stub.resetCounters();
        SourceRecorder rec2;
        rec2.attach( src );
        const int flatId =
            src.search( QString(), QString(), /*recursive=*/false, smedia::kAudio );
        check( waitFor( [&] { return rec2.sawFinal( flatId ); } ),
               QStringLiteral( "AC 3: the non-recursive search completes" ) );
        checkEq( stub.totalRequests(), 1,
                QStringLiteral( "AC 3: non-recursive issues exactly one PROPFIND" ) );
    }

    // ================================================================ AC 3
    // (continued) The kMaxSearchEntries cap: stop, announce a lower-bound
    // truncatedCount, and log a line naming the bound.
    {
        stub.clearDirectories();
        stub.resetCounters();

        const int kOverBy = 10;
        QVector<SWebDavStubEntry> many;
        many.reserve( smedia::kMaxSearchEntries + kOverBy );
        for( int i = 0; i < smedia::kMaxSearchEntries + kOverBy; ++i ) {
            SWebDavStubEntry e;
            e.name    = QStringLiteral( "f%1.wav" ).arg( i, 5, 10, QLatin1Char( '0' ) );
            e.content = QByteArray( 1, 'a' );
            many.append( e );
        }
        stub.setDirectory( QStringLiteral( "big" ), many );

        SWebDavMediaSource src( QStringLiteral( "acct1" ), base, QString() );
        SourceRecorder      rec;
        rec.attach( src );

        const uint64_t logFrom = tw::TwLog::instance().nextSeq();

        const int id = src.search( QStringLiteral( "big" ), QString(),
                                   /*recursive=*/false, smedia::kAudio );
        check( waitFor( [&] { return rec.sawFinal( id ); }, 30000 ),
               QStringLiteral( "AC 3: the over-cap search completes" ) );

        checkEq( rec.entriesFor( id ).size(), smedia::kMaxSearchEntries,
                QStringLiteral( "AC 3: delivered entries stop exactly at "
                                "kMaxSearchEntries" ) );
        check( rec.truncatedOnFinal( id ) > 0,
               QStringLiteral( "AC 3: truncatedCount > 0 on the final batch" ),
               QString::number( rec.truncatedOnFinal( id ) ) );

        std::vector<tw::LogRecord> logRecords;
        tw::TwLog::instance().snapshot( logFrom, tw::TwLog::instance().nextSeq(), logRecords );
        bool sawBoundLine = false;
        for( const tw::LogRecord &r : logRecords ) {
            if( r.text.find( "caps:" ) != std::string::npos
                && r.text.find( "5000 entries" ) != std::string::npos ) {
                sawBoundLine = true;
                break;
            }
        }
        check( sawBoundLine,
               QStringLiteral( "AC 3: a log line names the bound (never silent)" ) );
    }

    // ================================================================ AC 4
    // Errors: 401 does not retry; 404 fails cleanly; a mid-body close fails
    // the fetch and leaves no partial file.
    {
        stub.clearDirectories();
        stub.clearFaults();
        stub.resetCounters();

        SWebDavStubEntry note;
        note.name    = QStringLiteral( "secret.wav" );
        note.content = QByteArray( 32, 'z' );
        stub.setDirectory( QString(), { note } );

        stub.setFault( QStringLiteral( "locked" ), SWebDavStub::Fault::Status401 );

        SWebDavMediaSource src( QStringLiteral( "acct1" ), base, QString() );
        SourceRecorder      rec;
        rec.attach( src );

        const int id401 = src.listDirectory( QStringLiteral( "locked" ) );
        check( waitFor( [&] { return rec.hasFailure( id401 ); } ),
               QStringLiteral( "AC 4: a 401 fails the request" ) );
        checkEq( stub.requestCount( QStringLiteral( "locked" ) ), 1,
                QStringLiteral( "AC 4: ... exactly once -- no retry" ) );

        stub.resetCounters();
        const int id404 = src.listDirectory( QStringLiteral( "does-not-exist" ) );
        check( waitFor( [&] { return rec.hasFailure( id404 ); } ),
               QStringLiteral( "AC 4: a 404 fails the request, not a crash" ) );

        // Mid-body close leaves no partial file.
        QTemporaryDir cacheDir;
        check( cacheDir.isValid(), QStringLiteral( "AC 4: temp cache dir created" ) );
        const QString destPath = cacheDir.filePath( QStringLiteral( "secret.wav" ) );

        stub.setGetChunkCount( 4 );
        stub.setFault( QStringLiteral( "secret.wav" ), SWebDavStub::Fault::CloseMidBody );

        const int idGet = src.fetch( QStringLiteral( "secret.wav" ), destPath );
        check( waitFor( [&] { return rec.hasFailure( idGet ); } ),
               QStringLiteral( "AC 4: a connection closed mid-body fails the fetch" ) );

        const QStringList leftover =
            QDir( cacheDir.path() ).entryList( QDir::Files | QDir::Hidden | QDir::System );
        check( leftover.isEmpty(),
               QStringLiteral( "AC 4/5: no partial file and no .tmp left behind" ),
               leftover.join( ", " ) );

        stub.setFault( QStringLiteral( "secret.wav" ), SWebDavStub::Fault::None );
        stub.setGetChunkCount( 1 );
    }

    // ================================================================ AC 5
    // Cancel: a cancelled GET leaves no file and no .tmp behind.
    {
        stub.clearFaults();
        stub.setResponseDelayMs( 300 );   // give the test time to cancel first

        SWebDavMediaSource src( QStringLiteral( "acct1" ), base, QString() );
        SourceRecorder      rec;
        rec.attach( src );

        QTemporaryDir cacheDir;
        const QString destPath = cacheDir.filePath( QStringLiteral( "secret.wav" ) );

        const int id = src.fetch( QStringLiteral( "secret.wav" ), destPath );
        src.cancel( id );

        pump( 800 );   // let the (already-cancelled) response, if any, arrive and be dropped

        check( !rec.hasFailure( id ) && rec.fetched.isEmpty(),
               QStringLiteral( "AC 5: a cancelled fetch delivers no result at all" ) );
        const QStringList leftover =
            QDir( cacheDir.path() ).entryList( QDir::Files | QDir::Hidden | QDir::System );
        check( leftover.isEmpty(),
               QStringLiteral( "AC 5: no partial file and no .tmp behind a cancel" ),
               leftover.join( ", " ) );

        stub.setResponseDelayMs( 0 );
    }

    // ================================================================ AC 6
    // fetchProgress at least twice, monotonically, for a body large enough
    // to arrive in several chunks; the fetch itself completes byte-exact.
    {
        stub.clearDirectories();
        stub.clearFaults();

        QByteArray bigContent;
        bigContent.reserve( 200000 );
        for( int i = 0; i < 200000; ++i ) bigContent.append( char( 'A' + ( i % 26 ) ) );

        SWebDavStubEntry big;
        big.name    = QStringLiteral( "big.wav" );
        big.content = bigContent;
        stub.setDirectory( QString(), { big } );
        stub.setGetChunkCount( 8 );

        SWebDavMediaSource src( QStringLiteral( "acct1" ), base, QString() );
        SourceRecorder      rec;
        rec.attach( src );

        QTemporaryDir cacheDir;
        const QString destPath = cacheDir.filePath( QStringLiteral( "big.wav" ) );
        const int      id      = src.fetch( QStringLiteral( "big.wav" ), destPath );

        check( waitFor( [&] { return !rec.fetched.isEmpty(); }, 15000 ),
               QStringLiteral( "AC 6: the fetch completes" ) );

        check( rec.progressList.size() >= 2,
               QStringLiteral( "AC 6: fetchProgress fires at least twice" ),
               QString::number( rec.progressList.size() ) );

        bool monotone = true;
        qint64 prevDone = -1;
        for( const SourceRecorder::Progress &p : rec.progressList ) {
            if( p.id != id ) continue;
            if( p.done < prevDone ) monotone = false;
            prevDone = p.done;
        }
        check( monotone, QStringLiteral( "AC 6: fetchProgress is monotone" ) );

        if( !rec.fetched.isEmpty() ) {
            checkEq( rec.fetched.last().localPath, destPath,
                    QStringLiteral( "AC 6: fetchFinished reports destPath" ) );
            QFile downloaded( destPath );
            check( downloaded.open( QIODevice::ReadOnly )
                       && downloaded.readAll() == bigContent,
                   QStringLiteral( "AC 6: the downloaded bytes are byte-exact" ) );
        }
        stub.setGetChunkCount( 1 );
    }

    // ================================================================ AC 7
    // Destroying the source with requests in flight aborts them and does
    // not crash.
    {
        stub.clearDirectories();
        stub.clearFaults();
        stub.setResponseDelayMs( 300 );

        SWebDavStubEntry big;
        big.name    = QStringLiteral( "in-flight.wav" );
        big.content = QByteArray( 4096, 'q' );
        stub.setDirectory( QString(), { big } );

        QTemporaryDir cacheDir;
        {
            SWebDavMediaSource src( QStringLiteral( "acct1" ), base, QString() );
            src.listDirectory( QString() );
            src.fetch( QStringLiteral( "in-flight.wav" ),
                      cacheDir.filePath( QStringLiteral( "in-flight.wav" ) ) );
            // src is destroyed here, at scope exit, with both requests still
            // outstanding (the stub is holding them for 300ms).
        }
        pump( 500 );
        ok( QStringLiteral( "AC 7: destroying a source mid-request did not crash "
                            "(reaching this line is the assertion)" ) );

        stub.setResponseDelayMs( 0 );
    }

    // ================================================================ AC 8
    // Built by concatenation, exactly like gate 1's "no raw OS thread" grep
    // (media_source_test.cpp) -- so the banned QNetworkReply method name does
    // not appear contiguously in THIS file's own source and match itself
    // under the tree-wide grep the PR body quotes.
    {
        const QString banned =
            QStringLiteral( "ignoreSsl" ) + QStringLiteral( "Errors" );
        QStringList hits;
        for( const QString &dir : { moduleDir(), testkitStubDir() } ) {
            for( const QString &p : sourceFilesUnder( dir ) ) {
                const QString text = readAll( p );
                if( text.contains( banned ) ) hits << p;
            }
        }
        check( hits.isEmpty(),
               QStringLiteral( "AC 8: the SSL-error-ignore call appears "
                               "nowhere in the connector or the stub" ),
               hits.join( ' ' ) );
    }

    // ================================================================ AC 9
    // Stated, not asserted: this binary is the ONLY thing that has ever
    // driven SWebDavMediaSource. No account, no accounts model, no Options
    // page, no qxa case exists yet -- see the PR body.
    ok( QStringLiteral( "AC 9: end-to-end app coverage is deferred to gate 5c "
                        "(stated in the PR body)" ) );

    stub.stop();

    std::cout << "\n" << g_checks << " checks.\n";
    if( g_failures.isEmpty() ) {
        std::cout << "PASS - all webdav connector gate-4 checks passed.\n";
        return 0;
    }
    std::cout << g_failures.size() << " failures:\n\n";
    for( const QString &f : g_failures ) std::cout << f.toStdString() << "\n\n";
    return 1;
}
