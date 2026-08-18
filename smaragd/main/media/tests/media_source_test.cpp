// Gate 1 of proposal 38: the media source ABI and the local provider.
//
// The twelve ACs of §C GATE 1, one section each. Three of them are greps over
// this module's own sources (no raw OS thread, no widget include, the CONTRACT
// states its invariants) and they run HERE rather than being left to a PR
// body, because a claim nobody executes is a claim that rots.
//
// Two properties are inherently timing-shaped and are written so that they are
// FACTS rather than coin flips:
//
//   * "at most one further batch after cancel" (AC 7) and "nothing after the
//     source is destroyed" (AC 12) are both counts taken strictly after the
//     event, and both are ZERO by construction — the source drops batches for
//     a request that is no longer live, and Qt drops a queued emission whose
//     receiver has died. The AC's own number is what is asserted; the measured
//     value is printed beside it.
//   * The batching AC (5) needs more than 200 entries, which the committed
//     five-entry fixture cannot supply, so it builds a 600-file tree in a
//     QTemporaryDir. That gates the REAL batching rule instead of a test-only
//     knob.

#include "app/media/slocalmediasource.h"
#include "app/media/smediaref.h"
#include "app/media/smediaregistry.h"
#include "app/media/smediasource.h"
#include "app/media/smediatypes.h"

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

#ifndef SMEDIA_FIXTURE_DIR
#error "SMEDIA_FIXTURE_DIR must be supplied by the build (smaragd/tests/media)"
#endif
#ifndef SMEDIA_MODULE_DIR
#error "SMEDIA_MODULE_DIR must be supplied by the build (smaragd/main/media)"
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

void checkEq( int got, int expected, const QString &what )
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

// Everything a source emitted, in arrival order, with the thread it arrived on
// (AC 8) and a monotone sequence number so "after the cancel point" is a fact
// and not a guess.
struct Recorder
{
    struct Batch
    {
        qint64               seq = 0;
        int                  requestId = 0;
        QVector<SMediaEntry> entries;
        bool                 final = false;
        int                  truncated = 0;
        bool                 onMainThread = false;
    };
    struct Failure
    {
        qint64  seq = 0;
        int     requestId = 0;
        QString message;
        bool    onMainThread = false;
    };
    struct Fetched
    {
        qint64  seq = 0;
        int     requestId = 0;
        QString localPath;
        bool    onMainThread = false;
    };

    QVector<Batch>   batches;
    QVector<Failure> failures;
    QVector<Fetched> fetched;
    qint64           seq = 0;

    void clear()
    {
        batches.clear();
        failures.clear();
        fetched.clear();
        // seq deliberately keeps counting: it is the only ordering the test
        // has between an event it caused and an emission it received.
    }

    bool everOffMainThread() const
    {
        for( const Batch &b : batches )   if( !b.onMainThread ) return true;
        for( const Failure &f : failures ) if( !f.onMainThread ) return true;
        for( const Fetched &f : fetched )  if( !f.onMainThread ) return true;
        return false;
    }

    int batchCount( int requestId ) const
    {
        int n = 0;
        for( const Batch &b : batches ) if( b.requestId == requestId ) ++n;
        return n;
    }

    int finalCount( int requestId ) const
    {
        int n = 0;
        for( const Batch &b : batches ) {
            if( b.requestId == requestId && b.final ) ++n;
        }
        return n;
    }

    bool sawFinal( int requestId ) const { return finalCount( requestId ) > 0; }

    QStringList names( int requestId ) const
    {
        QStringList out;
        for( const Batch &b : batches ) {
            if( b.requestId != requestId ) continue;
            for( const SMediaEntry &e : b.entries ) out << e.name;
        }
        return out;
    }

    QVector<SMediaEntry> entries( int requestId ) const
    {
        QVector<SMediaEntry> out;
        for( const Batch &b : batches ) {
            if( b.requestId != requestId ) continue;
            out += b.entries;
        }
        return out;
    }

    int batchesAfter( qint64 mark, int requestId ) const
    {
        int n = 0;
        for( const Batch &b : batches ) {
            if( b.seq > mark && b.requestId == requestId ) ++n;
        }
        return n;
    }

    int finalsAfter( qint64 mark, int requestId ) const
    {
        int n = 0;
        for( const Batch &b : batches ) {
            if( b.seq > mark && b.requestId == requestId && b.final ) ++n;
        }
        return n;
    }

    int anyDeliveryAfter( qint64 mark ) const
    {
        int n = 0;
        for( const Batch &b : batches )    if( b.seq > mark ) ++n;
        for( const Failure &f : failures ) if( f.seq > mark ) ++n;
        for( const Fetched &f : fetched )  if( f.seq > mark ) ++n;
        return n;
    }
};

Recorder g_rec;
QObject *g_ctx = nullptr;   // the connection context; outlives every source

bool onMainThread()
{
    return QThread::currentThread() == QCoreApplication::instance()->thread();
}

void attach( SMediaSource *src )
{
    QObject::connect( src, &SMediaSource::entriesReady, g_ctx,
                      []( int id, const QVector<SMediaEntry> &batch, bool final,
                          int truncated ) {
                          Recorder::Batch b;
                          b.seq          = ++g_rec.seq;
                          b.requestId    = id;
                          b.entries      = batch;
                          b.final        = final;
                          b.truncated    = truncated;
                          b.onMainThread = onMainThread();
                          g_rec.batches.append( b );
                      } );
    QObject::connect( src, &SMediaSource::requestFailed, g_ctx,
                      []( int id, const QString &message ) {
                          Recorder::Failure f;
                          f.seq          = ++g_rec.seq;
                          f.requestId    = id;
                          f.message      = message;
                          f.onMainThread = onMainThread();
                          g_rec.failures.append( f );
                      } );
    QObject::connect( src, &SMediaSource::fetchFinished, g_ctx,
                      []( int id, const QString &localPath ) {
                          Recorder::Fetched f;
                          f.seq          = ++g_rec.seq;
                          f.requestId    = id;
                          f.localPath    = localPath;
                          f.onMainThread = onMainThread();
                          g_rec.fetched.append( f );
                      } );
}

// Spin the main event loop until `pred` holds or the budget runs out. Never a
// blind sleep: every wait in this file is on an observable condition.
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

// Drain whatever is already queued, without waiting on a condition. Used only
// where the point IS that nothing more should arrive.
void pump( int ms )
{
    QElapsedTimer t;
    t.start();
    while( t.elapsed() < ms ) {
        QCoreApplication::processEvents( QEventLoop::AllEvents, 5 );
        QThread::msleep( 1 );
    }
}

QString fixtureRoot()
{
    return QDir::fromNativeSeparators(
        QDir::cleanPath( QStringLiteral( SMEDIA_FIXTURE_DIR ) + "/lib" ) );
}

QString moduleDir()
{
    return QDir::fromNativeSeparators(
        QDir::cleanPath( QStringLiteral( SMEDIA_MODULE_DIR ) ) );
}

QStringList moduleSources()
{
    QStringList out;
    QDir root( moduleDir() );
    QVector<QString> stack{ root.absolutePath() };
    while( !stack.isEmpty() ) {
        const QString here = stack.takeLast();
        QDir          d( here );
        const QFileInfoList infos =
            d.entryInfoList( QDir::AllEntries | QDir::NoDotAndDotDot, QDir::Name );
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

// Builds a tree big enough for the batching, cancel, supersession and
// destroy-mid-walk cases: 5 directories x 400 files, all matching the Audio
// filter, plus one non-matching file per directory so the filter is exercised
// off the committed fixture too. 400 rather than the 200 one batch holds,
// because the point of every case below is that the walk is still running
// when something happens to it.
struct BigTree
{
    QTemporaryDir dir;
    int           wavCount = 0;

    bool build()
    {
        if( !dir.isValid() ) return false;
        QDir root( dir.path() );
        for( int d = 0; d < 5; ++d ) {
            const QString sub = QStringLiteral( "d%1" ).arg( d );
            if( !root.mkpath( sub ) ) return false;
            QDir here( root.absoluteFilePath( sub ) );
            for( int i = 0; i < 400; ++i ) {
                QFile f( here.absoluteFilePath(
                    QStringLiteral( "loop%1.wav" ).arg( i, 4, 10,
                                                        QLatin1Char( '0' ) ) ) );
                if( !f.open( QIODevice::WriteOnly ) ) return false;
                f.write( "RIFF" );
                f.close();
                ++wavCount;
            }
            QFile note( here.absoluteFilePath( QStringLiteral( "readme.txt" ) ) );
            if( note.open( QIODevice::WriteOnly ) ) note.close();
        }
        return true;
    }

    QString path() const { return QDir::fromNativeSeparators( dir.path() ); }
};

} // namespace

int main( int argc, char **argv )
{
    QCoreApplication app( argc, argv );
    QObject ctx;
    g_ctx = &ctx;

    const QString fixture = fixtureRoot();
    std::cout << "fixture root: " << fixture.toStdString() << "\n";
    std::cout << "module  dir : " << moduleDir().toStdString() << "\n\n";

    check( QFileInfo( fixture ).isDir(),
           QStringLiteral( "the committed fixture tree exists" ), fixture );

    // ------------------------------------------------------------------ URI
    // SMediaRef is the identity every later gate quotes; a round trip that
    // loses a Windows drive letter or a colon-bearing source id would be found
    // three gates from here.
    {
        SMediaRef r;
        r.sourceId = QStringLiteral( "local" );
        r.path     = QStringLiteral( "C:/samples/kick.wav" );
        checkEq( r.toUri(), QStringLiteral( "smedia://local/C:/samples/kick.wav" ),
                 QStringLiteral( "toUri spells a local Windows path" ) );

        const SMediaRef back = SMediaRef::fromUri( r.toUri() );
        check( back == r, QStringLiteral( "fromUri round-trips a local ref" ),
               back.sourceId + " | " + back.path );

        SMediaRef remote;
        remote.sourceId = QStringLiteral( "nextcloud:acct1" );
        remote.path     = QStringLiteral( "Drum Kit/kick#1.wav" );
        const SMediaRef remoteBack = SMediaRef::fromUri( remote.toUri() );
        check( remoteBack == remote,
               QStringLiteral( "a colon-bearing source id survives the URI" ),
               remoteBack.sourceId + " | " + remoteBack.path );

        check( !SMediaRef::fromUri( QStringLiteral( "file:/x.wav" ) ).isValid(),
               QStringLiteral( "a non-smedia URI yields an invalid ref" ) );
    }

    // ------------------------------------------------------- the type table
    // §B.3/§G.2: kAudio IS the Insert-sample dialog's seven suffixes. Gate 2
    // builds that dialog string FROM this list, so a drift here is a silent
    // change to a shipping dialog.
    {
        const QStringList expected = { "wav", "mp3", "flac", "aiff",
                                       "aif", "ogg", "opus" };
        check( smedia::kAudio == expected,
               QStringLiteral( "kAudio is the dialog's exact seven suffixes" ),
               smedia::kAudio.join( ' ' ) );
        check( !smedia::kAudio.contains( QStringLiteral( "oga" ) ),
               QStringLiteral( "kAudio does NOT contain 'oga' (§G.2)" ) );
        check( !smedia::kAudio.contains( QStringLiteral( "m4a" ) ),
               QStringLiteral( "kAudio does NOT contain 'm4a' (undecodable)" ) );
        check( smedia::suffixesFor( smedia::Audio ) == expected,
               QStringLiteral( "suffixesFor(Audio) == kAudio" ) );
        check( smedia::suffixesFor( 0 ).isEmpty(),
               QStringLiteral( "an empty mask yields an empty suffix list" ) );
        check( smedia::suffixAccepted( QStringLiteral( "x.WAV" ),
                                       smedia::kAudio ),
               QStringLiteral( "the suffix match is case-insensitive" ) );
        check( smedia::suffixAccepted( QStringLiteral( "x.txt" ), {} ),
               QStringLiteral( "an empty filter accepts everything" ) );
        checkEq( smedia::categoryMaskToString( smedia::Audio | smedia::Midi ),
                 QStringLiteral( "audio,midi" ),
                 QStringLiteral( "the mask has one canonical spelling" ) );
        checkEq( smedia::categoryMaskFromString( QStringLiteral( "audio" ) ),
                 int( smedia::Audio ),
                 QStringLiteral( "the mask parses back" ) );
    }

    // -------------------------------------------------------------- AC 2/8
    // listDirectory: directories before files, each sorted by name, with
    // isDir / sizeBytes / modified populated.
    {
        auto *src = new SLocalMediaSource();
        attach( src );
        g_rec.clear();

        const int id = src->listDirectory( fixture );
        check( id > 0, QStringLiteral( "listDirectory returns a request id" ) );
        check( waitFor( [&] { return g_rec.sawFinal( id ); } ),
               QStringLiteral( "AC 2: the listing completes" ) );

        const QStringList got = g_rec.names( id );
        const QStringList expected = { "sub", "kick.wav", "notes.txt",
                                       "snare.wav" };
        check( got == expected,
               QStringLiteral( "AC 2: directories first, then files, "
                               "each by name" ),
               got.join( ' ' ) );

        const QVector<SMediaEntry> entries = g_rec.entries( id );
        bool shapeOk = entries.size() == 4;
        for( const SMediaEntry &e : entries ) {
            if( e.name == QLatin1String( "sub" ) ) {
                shapeOk = shapeOk && e.isDir && e.sizeBytes == -1;
            } else {
                shapeOk = shapeOk && !e.isDir && e.sizeBytes > 0;
            }
            shapeOk = shapeOk && e.modified.isValid();
            shapeOk = shapeOk && e.ref.sourceId == QLatin1String( "local" );
            shapeOk = shapeOk && e.ref.path.endsWith( e.name );
            shapeOk = shapeOk && e.etag.isEmpty();
        }
        check( shapeOk,
               QStringLiteral( "AC 2: isDir / sizeBytes / modified / ref are "
                               "populated (a dir's size is the documented -1)" ) );

        // The wav fixtures are byte copies of test_sawtooth.wav, so the size
        // is a closed form rather than a measurement.
        qint64 kickSize = -1;
        for( const SMediaEntry &e : entries ) {
            if( e.name == QLatin1String( "kick.wav" ) ) kickSize = e.sizeBytes;
        }
        checkEq( int( kickSize ), 768044,
                 QStringLiteral( "AC 2: kick.wav's size is the fixture's" ) );

        // A non-existent path FAILS; it never reports an empty success.
        g_rec.clear();
        const int badId = src->listDirectory( fixture + "/does-not-exist" );
        check( waitFor( [&] { return !g_rec.failures.isEmpty(); } ),
               QStringLiteral( "AC 2: a non-existent path emits requestFailed" ) );
        checkEq( g_rec.batchCount( badId ), 0,
                 QStringLiteral( "AC 2: ... and no empty success batch" ) );

        // ------------------------------------------------------------ AC 3
        g_rec.clear();
        src->setSuffixFilter( smedia::suffixesFor( smedia::Audio ) );
        const int filtered = src->listDirectory( fixture );
        check( waitFor( [&] { return g_rec.sawFinal( filtered ); } ),
               QStringLiteral( "AC 3: the filtered listing completes" ) );
        const QStringList filteredNames = g_rec.names( filtered );
        check( filteredNames == QStringList( { "sub", "kick.wav", "snare.wav" } ),
               QStringLiteral( "AC 3: the Audio mask omits notes.txt and keeps "
                               "the directory" ),
               filteredNames.join( ' ' ) );
        src->setSuffixFilter( {} );

        // ---------------------------------------------------------- fetch()
        g_rec.clear();
        const int fetchId =
            src->fetch( fixture + "/kick.wav", QString() );
        check( waitFor( [&] { return !g_rec.fetched.isEmpty(); } ),
               QStringLiteral( "fetch of a local file finishes asynchronously" ) );
        check( !g_rec.fetched.isEmpty()
                   && g_rec.fetched.first().requestId == fetchId
                   && g_rec.fetched.first().localPath
                          == fixture + "/kick.wav",
               QStringLiteral( "fetch hands back the path it was given" ) );

        g_rec.clear();
        src->fetch( fixture + "/nope.wav", QString() );
        check( waitFor( [&] { return !g_rec.failures.isEmpty(); } ),
               QStringLiteral( "fetch of a missing file fails, never succeeds "
                               "with an empty path" ) );

        check( !g_rec.everOffMainThread(),
               QStringLiteral( "AC 8: every emission so far arrived on the "
                               "MAIN thread" ) );
        delete src;
    }

    // ---------------------------------------------------------------- AC 4
    // The exact counts against the committed fixture. The needle "a" matches
    // every wav because the SUFFIX contains it — the matcher looks at the
    // whole name, which is what a search box means — so the discriminating
    // needles below are what prove it is not matching everything blindly.
    {
        auto *src = new SLocalMediaSource();
        attach( src );

        struct Case { QString needle; bool recursive; int expect; const char *why; };
        const QVector<Case> cases = {
            { QStringLiteral( "a" ), true,  4, "recursive 'a' finds all four wavs" },
            { QStringLiteral( "a" ), false, 2, "non-recursive 'a' finds the two in lib/" },
            { QStringLiteral( "sn" ), true,  1, "'sn' finds snare.wav alone" },
            { QStringLiteral( "tom" ), true, 1, "'tom' reaches depth 2" },
            { QStringLiteral( "tom" ), false, 0, "'tom' is out of reach non-recursively" },
            { QStringLiteral( "zzz" ), true, 0, "a needle that matches nothing finds nothing" },
            { QString(),             true,  4, "an empty needle is every audio file" },
        };
        for( const Case &c : cases ) {
            g_rec.clear();
            const int id = src->search( fixture, c.needle, c.recursive,
                                        smedia::suffixesFor( smedia::Audio ) );
            check( waitFor( [&] { return g_rec.sawFinal( id ); } ),
                   QStringLiteral( "AC 4: search('%1', recursive=%2) completes" )
                       .arg( c.needle.isEmpty() ? QStringLiteral( "<empty>" )
                                                : c.needle )
                       .arg( c.recursive ? 1 : 0 ) );
            checkEq( g_rec.entries( id ).size(), c.expect,
                     QStringLiteral( "AC 4: %1" )
                         .arg( QLatin1String( c.why ) ) );
        }

        // A search reports FILES only — notes.txt is filtered by the suffix
        // list, and `sub` is a container to walk, not a result.
        g_rec.clear();
        const int all = src->search( fixture, QString(), true, {} );
        check( waitFor( [&] { return g_rec.sawFinal( all ); } ),
               QStringLiteral( "AC 4: an unfiltered search completes" ) );
        QStringList allNames = g_rec.names( all );
        allNames.sort();
        check( allNames == QStringList( { "hat.wav", "kick.wav", "notes.txt",
                                          "snare.wav", "tom.wav" } ),
               QStringLiteral( "AC 4: an unfiltered search returns the five "
                               "FILES and no directory" ),
               allNames.join( ' ' ) );

        check( !g_rec.everOffMainThread(),
               QStringLiteral( "AC 8: every search emission arrived on the "
                               "MAIN thread" ) );
        delete src;
    }

    // ---- the bigger tree the timing-shaped ACs need -------------------------
    BigTree big;
    check( big.build(),
           QStringLiteral( "a %1-file temporary tree was built" )
               .arg( big.wavCount ),
           big.dir.path() );

    // ---------------------------------------------------------------- AC 5
    {
        auto *src = new SLocalMediaSource();
        attach( src );
        g_rec.clear();

        const int id = src->search( big.path(), QString(), true,
                                    smedia::suffixesFor( smedia::Audio ) );
        check( waitFor( [&] { return g_rec.sawFinal( id ); } ),
               QStringLiteral( "AC 5: the 2000-entry search completes" ) );

        const int batches = g_rec.batchCount( id );
        check( batches > 1,
               QStringLiteral( "AC 5: it took more than one batch (got %1)" )
                   .arg( batches ) );
        checkEq( g_rec.finalCount( id ), 1,
                 QStringLiteral( "AC 5: final=true exactly once" ) );
        checkEq( g_rec.entries( id ).size(), big.wavCount,
                 QStringLiteral( "AC 5: the union of the batches is the whole "
                                 "result set" ) );

        // Every non-final batch precedes the final one: streaming, not a
        // single dump with a flag on it.
        bool finalWasLast = true;
        bool seenFinal    = false;
        for( const Recorder::Batch &b : g_rec.batches ) {
            if( b.requestId != id ) continue;
            if( seenFinal ) finalWasLast = false;
            if( b.final ) seenFinal = true;
        }
        check( finalWasLast,
               QStringLiteral( "AC 5: nothing follows the final batch" ) );

        int truncatedOnFinal = -1;
        for( const Recorder::Batch &b : g_rec.batches ) {
            if( b.requestId == id && b.final ) truncatedOnFinal = b.truncated;
        }
        checkEq( truncatedOnFinal, 0,
                 QStringLiteral( "AC 5: a complete walk announces no "
                                 "truncation" ) );
        delete src;
    }

    // -------------------------------------------------------------- AC 6/7
    {
        auto *src = new SLocalMediaSource();
        attach( src );
        g_rec.clear();

        // Two overlapping searches; the first is then dropped by id.
        const int stale  = src->search( big.path(), QString(), true,
                                        smedia::suffixesFor( smedia::Audio ) );
        const int newest = src->search( big.path(),
                                        QStringLiteral( "loop000" ), true,
                                        smedia::suffixesFor( smedia::Audio ) );
        check( stale != newest && newest > stale,
               QStringLiteral( "AC 6: request ids are distinct and monotone" ) );

        // The cancel happens in the SAME event-loop turn both searches were
        // issued in, so the first walk is UNAMBIGUOUSLY still running: nothing
        // has pumped, so nothing can have been delivered or completed. Waiting
        // for a batch first would have been the natural way to write this and
        // is not reliable -- measured on this box, all eleven batches of a
        // 2000-file walk arrive inside a single processEvents pass, so the
        // "wait, then cancel" version cancelled a walk that had already
        // finished and asserted nothing.
        const qint64 cancelPoint = g_rec.seq;
        src->cancel( stale );
        src->cancel( 999999 );   // AC 7: an unknown id is a no-op

        check( waitFor( [&] { return g_rec.sawFinal( newest ); } ),
               QStringLiteral( "AC 6: the newer search still completes" ) );
        pump( 300 );

        const int after = g_rec.batchesAfter( cancelPoint, stale );
        check( after <= 1,
               QStringLiteral( "AC 6/7: at most ONE further batch carried the "
                               "cancelled id after the cancel point "
                               "(measured %1)" ).arg( after ),
               QStringLiteral( "measured %1" ).arg( after ) );
        // A COMPLETED walk legitimately reported final=true BEFORE the
        // cancel; what must never happen is a completion AFTER it. Asserting
        // finalCount == 0 outright would be asserting that the box is slow.
        checkEq( g_rec.finalsAfter( cancelPoint, stale ), 0,
                 QStringLiteral( "AC 7: a cancelled request reports no "
                                 "final=true after the cancel point" ) );
        checkEq( g_rec.entries( newest ).size(), 50,
                 QStringLiteral( "AC 6: the surviving search's own result is "
                                 "intact (loop000[0-9].wav, 10 per dir x 5)" ) );
        checkEq( src->liveRequestCount(), 0,
                 QStringLiteral( "AC 7: no request is left live" ) );

        // The DETERMINISTIC half of AC 7, which does not depend on how fast
        // the box walks 2000 files: cancel in the SAME event-loop turn the
        // search was issued in, before anything could possibly have been
        // delivered. Nothing at all may arrive for that id.
        g_rec.clear();
        const qint64 immMark = g_rec.seq;
        const int    imm =
            src->search( big.path(), QString(), true,
                         smedia::suffixesFor( smedia::Audio ) );
        src->cancel( imm );
        pump( 800 );
        checkEq( g_rec.batchCount( imm ), 0,
                 QStringLiteral( "AC 7: a walk cancelled before its first "
                                 "delivery delivers NOTHING" ) );
        checkEq( g_rec.anyDeliveryAfter( immMark ), 0,
                 QStringLiteral( "AC 7: ... and emits no failure either" ) );

        check( !g_rec.everOffMainThread(),
               QStringLiteral( "AC 8: every emission arrived on the MAIN "
                               "thread" ) );
        delete src;
    }

    // --------------------------------------------------------------- AC 12
    // The one that separates the queued signal/slot hand-back from the
    // raw-pointer invokeMethod that looks identical until the day it loses.
    // Variant A: destroyed in the SAME event-loop turn the search was issued
    // in. The walk cannot have finished -- nothing has pumped -- so the pool
    // thread is enumerating (or about to) while the receiver is torn down.
    {
        auto *src = new SLocalMediaSource();
        attach( src );
        g_rec.clear();

        const qint64 mark = g_rec.seq;
        src->search( big.path(), QString(), true,
                     smedia::suffixesFor( smedia::Audio ) );
        delete src;                 // <- the receiver dies under the walk
        pump( 1500 );               // let the pool finish and try to deliver

        checkEq( g_rec.anyDeliveryAfter( mark ), 0,
                 QStringLiteral( "AC 12a: a source destroyed in the same turn "
                                 "its walk started delivers NOTHING" ) );
        ok( QStringLiteral( "AC 12a: ... and did not crash" ) );
    }

    // Variant B: destroyed after the first batch has actually arrived, so the
    // walk is demonstrably producing. Whether a batch is in flight at the
    // exact instant of the delete is the box's business -- which is the whole
    // point: the safe and the unsafe hand-back are indistinguishable on a
    // machine that never loses the race, so the CODE SHAPE is what protects
    // this and the case exists to execute the scenario at all.
    {
        auto *src = new SLocalMediaSource();
        attach( src );
        g_rec.clear();

        const int id = src->search( big.path(), QString(), true,
                                    smedia::suffixesFor( smedia::Audio ) );
        const bool started =
            waitFor( [&] { return g_rec.batchCount( id ) > 0; }, 5000 );
        const bool inFlight = started && !g_rec.sawFinal( id );

        const qint64 mark = g_rec.seq;
        delete src;
        pump( 1500 );

        checkEq( g_rec.anyDeliveryAfter( mark ), 0,
                 QStringLiteral( "AC 12b: a source destroyed mid-walk delivers "
                                 "NOTHING afterwards (walk was %1)" )
                     .arg( inFlight ? QStringLiteral( "in flight" )
                                    : QStringLiteral( "already finished" ) ) );
        ok( QStringLiteral( "AC 12b: ... and did not crash" ) );
    }

    // ------------------------------------------------------------- registry
    {
        SMediaRegistry &reg = SMediaRegistry::instance();
        checkEq( reg.count(), 0,
                 QStringLiteral( "the registry starts empty" ) );

        auto *src = new SLocalMediaSource();
        reg.registerSource( src );
        check( reg.source( QStringLiteral( "local" ) ) == src,
               QStringLiteral( "a registered source resolves by id" ) );
        check( reg.sourceIds() == QStringList( { "local" } ),
               QStringLiteral( "sourceIds lists it" ) );

        delete src;
        check( reg.source( QStringLiteral( "local" ) ) == nullptr,
               QStringLiteral( "the registry forgets a source that dies "
                               "(it does not own them)" ) );
        checkEq( reg.count(), 0,
                 QStringLiteral( "... and the count comes back to zero" ) );
    }

    // ------------------------------------------------------------ AC 9 / 11
    // Two greps, run here rather than promised in a PR body.
    {
        const QStringList sources = moduleSources();
        check( sources.size() >= 8,
               QStringLiteral( "the module's sources were found (%1 files)" )
                   .arg( sources.size() ) );

        QStringList threadHits, widgetHits;
        // Assembled rather than written out, so that this file does not itself
        // become a hit for the grep AC 9 names.
        const QString rawThread =
            QStringLiteral( "std" ) + QStringLiteral( "::thread" );
        const QRegularExpression widgetRe(
            QStringLiteral( "#include\\s*<Q(Widget|TreeWidget|Dialog|Menu|"
                            "Painter|Pixmap)" ) );
        for( const QString &p : sources ) {
            if( p.contains( QStringLiteral( "/tests/" ) ) ) continue;
            const QString text = readAll( p );
            if( text.contains( rawThread ) ) threadHits << p;
            if( widgetRe.match( text ).hasMatch() ) widgetHits << p;
        }
        check( threadHits.isEmpty(),
               QStringLiteral( "AC 9: no raw OS thread anywhere in "
                               "main/media's sources" ),
               threadHits.join( ' ' ) );
        check( widgetHits.isEmpty(),
               QStringLiteral( "AC 11: no widget header included in a "
                               "provider" ),
               widgetHits.join( ' ' ) );
    }

    // ---------------------------------------------------------------- AC 10
    {
        const QString contract = readAll( moduleDir() + "/CONTRACT.md" );
        check( !contract.isEmpty(),
               QStringLiteral( "AC 10: main/media/CONTRACT.md exists" ) );

        // The lead sentence of each of §B.2's six invariants, verbatim.
        const QStringList leads = {
            QStringLiteral( "**Everything is async and id-tagged.**" ),
            QStringLiteral( "**Supersession is by ID, never by a flag.**" ),
            QStringLiteral( "**Results STREAM.**" ),
            QStringLiteral( "**A truncation is ANNOUNCED, never silent.**" ),
            QStringLiteral( "**A provider never touches the model and never "
                            "submits an action.**" ),
            QStringLiteral( "**`cancel()` is advisory and results after it "
                            "must be safely droppable.**" ),
        };
        bool allPresent = true;
        for( const QString &lead : leads ) {
            if( !contract.contains( lead ) ) {
                allPresent = false;
                g_failures.append( "CONTRACT.md is missing: " + lead );
            }
        }
        check( allPresent,
               QStringLiteral( "AC 10: it states inv. 1-6 of §B.2 verbatim" ) );
        check( contract.contains(
                   QStringLiteral( "QMetaObject::invokeMethod` ON A RAW "
                                   "POINTER" ) ),
               QStringLiteral( "AC 10: ... including the hand-back rule that "
                               "AC 12 exists to enforce" ) );
    }

    std::cout << "\n" << g_checks << " checks.\n";
    if( g_failures.isEmpty() ) {
        std::cout << "PASS - all media source gate-1 checks passed.\n";
        return 0;
    }
    std::cout << g_failures.size() << " failures:\n\n";
    for( const QString &f : g_failures ) std::cout << f.toStdString() << "\n\n";
    return 1;
}
