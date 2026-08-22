#include "app/media/smediadrop.h"

#include "app/media/smediacache.h"
#include "app/media/smediaregistry.h"
#include "app/media/smediasource.h"

#include "app/model/sappcontext.h"
#include "app/model/sexternfile.h"
#include "app/model/sobject.h"
#include "app/model/sobjectpath.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"

#include "tw/core/twlog.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QSet>
#include <QVector>

namespace {

smediadrop::PlaceSampleFn g_place;
smediadrop::StatusFn      g_status;

int g_placed    = 0;
int g_failed    = 0;
int g_abandoned = 0;
int g_nextId    = 1;

// ONE pending placement. It holds the target's IDENTITY (§B.5 / T18) and the
// project it was made against — never an index-path, which is a POSITION in a
// tree the user is free to edit while the fetch runs.
struct Pending
{
    int                id = 0;
    SMediaRef          ref;
    QPointer<SObject>  track;
    QPointer<SProject> project;
    offset_t           timePos = 0;
    int                requestId = 0;
    QString            fetchPath;
    QString            displayName;
    QPointer<SMediaSource> source;
};

QVector<Pending> g_pending;

void say( const QString &message )
{
    if( g_status ) g_status( message );
}

int indexOfRequest( SMediaSource *source, int requestId )
{
    for( int i = 0; i < g_pending.size(); ++i )
        if( g_pending[ i ].source == source &&
            g_pending[ i ].requestId == requestId )
            return i;
    return -1;
}

QByteArray fileSha1( const QString &path )
{
    QFile f( path );
    if( !f.open( QIODevice::ReadOnly ) ) return QByteArray();
    QCryptographicHash h( QCryptographicHash::Sha1 );
    if( !h.addData( &f ) ) return QByteArray();
    return h.result();
}

// The project's own referenced files — the eviction PIN (T17). Exactly the set
// of files the project has a clip over, already maintained by the model.
QSet<QString> pinnedPaths( SProject *project )
{
    QSet<QString> out;
    if( !project ) return out;
    const auto &files = project->externFiles();
    for( auto it = files.constBegin(); it != files.constEnd(); ++it ) {
        if( !it.value() ) continue;
        const QString name = it.value()->getFileName();
        if( name.isEmpty() ) continue;
        out.insert( QFileInfo( name ).absoluteFilePath() );
    }
    return out;
}

// Everything that happens once a LOCAL path exists: the project copy, then the
// one add-sample. Returns true when a clip was placed.
bool finishPlacement( const Pending &p, const QString &localPath )
{
    SProject *current = SAppContext::get().getCurrentProject();
    if( !current || p.project.isNull() || p.project.data() != current ) {
        // Closed OR SWITCHED (§B.5). A close is one case of a change, not the
        // only one, so the test is pointer identity against the CURRENT one.
        ++g_abandoned;
        TW_LOGW( "media", "drop: the project changed while '%s' was fetching — "
                          "nothing placed",
                 p.ref.path.toUtf8().constData() );
        say( QObject::tr( "Media drop cancelled: the project changed." ) );
        return false;
    }
    // T18: is the target still IN THE TREE? Two separate ways it may not be,
    // and both must place nothing:
    //
    //   * the object is destroyed — the QPointer nulls, which a bare pointer or
    //     a serialized `id` could not tell you, and which an address the
    //     allocator handed to a NEW track could actively lie about;
    //   * the object is alive but DETACHED. `remove-track` is undoable and pins
    //     the removed track on the action object, so it outlives its own
    //     removal by design. Reachability from the root is therefore the
    //     question, not liveness.
    //
    // pathOf() returns {} for "the root itself" as well as "not found", and a
    // drop target is never the root — so empty means unreachable, exactly as
    // SMVActualView::dropEvent already reads it.
    const bool reachable =
        !p.track.isNull() &&
        !strackpath::pathOf( splacements::rootContainer( current ),
                             p.track.data() ).isEmpty();
    if( !reachable ) {
        ++g_abandoned;
        TW_LOGW( "media", "drop: the target track was removed while '%s' was "
                          "fetching — nothing placed",
                 p.ref.path.toUtf8().constData() );
        say( QObject::tr( "Media drop cancelled: the target track is gone." ) );
        return false;
    }

    const QString finalPath = smediadrop::materialiseIntoProject(
        localPath, p.displayName, current );

    // The cache is trimmed AFTER the project copy exists, so the copy's source
    // is pinned by the project the moment it can be.
    SMediaCache::instance().evictToCap( pinnedPaths( current ) );

    if( !g_place ) {
        ++g_failed;
        TW_LOGE( "media", "drop: no placement hook installed — '%s' was "
                          "fetched and nothing placed it",
                 finalPath.toUtf8().constData() );
        return false;
    }
    if( !g_place( p.track.data(), finalPath, p.timePos ) ) {
        ++g_failed;
        TW_LOGE( "media", "drop: the placement of '%s' was refused",
                 finalPath.toUtf8().constData() );
        say( QObject::tr( "Could not place %1." )
                 .arg( QFileInfo( finalPath ).fileName() ) );
        return false;
    }
    ++g_placed;
    return true;
}

void onFetchFinished( SMediaSource *source, int requestId,
                      const QString &localPath )
{
    const int idx = indexOfRequest( source, requestId );
    if( idx < 0 ) return;
    const Pending p = g_pending.takeAt( idx );

    QString error;
    const QString published =
        SMediaCache::instance().publish( p.ref, localPath, &error );
    if( published.isEmpty() ) {
        ++g_failed;
        TW_LOGE( "media", "drop: could not cache '%s': %s",
                 p.ref.path.toUtf8().constData(), error.toUtf8().constData() );
        say( QObject::tr( "Could not cache %1: %2" )
                 .arg( p.displayName, error ) );
        return;
    }
    finishPlacement( p, published );
}

void onRequestFailed( SMediaSource *source, int requestId,
                      const QString &message )
{
    const int idx = indexOfRequest( source, requestId );
    if( idx < 0 ) return;
    const Pending p = g_pending.takeAt( idx );
    if( !p.fetchPath.isEmpty() ) QFile::remove( p.fetchPath );
    ++g_failed;
    TW_LOGE( "media", "drop: fetch of '%s' failed: %s — nothing placed",
             p.ref.path.toUtf8().constData(), message.toUtf8().constData() );
    say( QObject::tr( "Could not fetch %1: %2" ).arg( p.displayName, message ) );
}

}   // namespace

namespace smediadrop {

void setPlacementHook( PlaceSampleFn fn ) { g_place = std::move( fn ); }
void setStatusHook( StatusFn fn ) { g_status = std::move( fn ); }

int  pendingCount()   { return g_pending.size(); }
int  placedCount()    { return g_placed; }
int  failedCount()    { return g_failed; }
int  abandonedCount() { return g_abandoned; }

void resetCounters()
{
    g_placed = g_failed = g_abandoned = 0;
    SMediaCache::instance().resetCounters();
}

void projectChanged()
{
    if( g_pending.isEmpty() ) return;
    TW_LOGI( "media", "drop: the project changed — %d pending placement(s) "
                      "dropped",
             (int) g_pending.size() );
    for( const Pending &p : g_pending ) {
        if( p.source ) p.source->cancel( p.requestId );
        if( !p.fetchPath.isEmpty() ) QFile::remove( p.fetchPath );
        ++g_abandoned;
    }
    g_pending.clear();
    say( QObject::tr( "Pending media drops cancelled." ) );
}

QString sanitiseFileName( const QString &name )
{
    // WebDAV permits ':', '?', '*', '|', a trailing dot and a trailing space in
    // a file name; Windows permits none of them, and this repo is tested on
    // Windows first. §B.6 / T19.
    static const QString illegal = QStringLiteral( "<>:\"/\\|?*" );
    QString out;
    out.reserve( name.size() );
    for( QChar c : name ) {
        if( c.unicode() < 0x20 || illegal.contains( c ) ) out += QLatin1Char( '_' );
        else                                              out += c;
    }
    while( !out.isEmpty() &&
           ( out.endsWith( QLatin1Char( '.' ) ) ||
             out.endsWith( QLatin1Char( ' ' ) ) ) )
        out.chop( 1 );
    out = out.trimmed();
    if( out.isEmpty() ) out = QStringLiteral( "media" );
    return out;
}

QString materialiseIntoProject( const QString &localPath,
                                const QString &displayName,
                                SProject *project )
{
    const QString projectFile = project ? project->projectFilePath() : QString();
    if( projectFile.isEmpty() ) {
        // T11, stated LOUDLY rather than discovered at the next machine: the
        // cache dir is machine-local, so a clip pointing into it serialises as
        // an absolute path that means nothing anywhere else.
        TW_LOGW( "media", "drop: the project has never been saved, so the clip "
                          "references the CACHE (%s). Save the project and "
                          "re-drop to get a portable copy.",
                 localPath.toUtf8().constData() );
        say( QObject::tr( "The project is unsaved: the clip references the "
                          "media cache, which will not travel to another "
                          "machine." ) );
        return localPath;
    }

    const QString mediaDir =
        QDir( QFileInfo( projectFile ).absolutePath() )
            .filePath( QStringLiteral( "media" ) );
    if( !QDir().mkpath( mediaDir ) ) {
        TW_LOGE( "media", "drop: could not create %s — the clip references the "
                          "cache instead",
                 mediaDir.toUtf8().constData() );
        return localPath;
    }

    QString base =
        sanitiseFileName( displayName.isEmpty()
                              ? QFileInfo( localPath ).fileName()
                              : displayName );
    QString stem   = QFileInfo( base ).completeBaseName();
    QString suffix = QFileInfo( base ).suffix();
    if( stem.isEmpty() ) { stem = base; suffix.clear(); }

    // CAP THE PATH so <projectdir>/media/<name> stays inside MAX_PATH on a
    // default Windows configuration. The room left over is for the " (99)" a
    // collision may add.
    // Never below 8: a project directory long enough to make `room` negative
    // has already blown the budget, and leaving the stem untruncated there is
    // strictly worse than shortening it as far as we can.
    const int room =
        qMax( 8, 255 - (int) mediaDir.size() - 1
                     - ( suffix.isEmpty() ? 0 : (int) suffix.size() + 1 ) - 5 );
    if( stem.size() > room ) stem = stem.left( room );
    if( stem.isEmpty() ) stem = QStringLiteral( "media" );

    const QByteArray wantHash = fileSha1( localPath );

    for( int n = 1; n < 1000; ++n ) {
        QString name = ( n == 1 )
                           ? stem
                           : stem + QStringLiteral( " (%1)" ).arg( n );
        if( !suffix.isEmpty() ) name += QLatin1Char( '.' ) + suffix;
        const QString target = QDir( mediaDir ).filePath( name );

        if( !QFileInfo::exists( target ) ) {
            if( !QFile::copy( localPath, target ) ) {
                TW_LOGE( "media", "drop: could not copy into %s — the clip "
                                  "references the cache instead",
                         target.toUtf8().constData() );
                return localPath;
            }
            TW_LOGI( "media", "drop: copied into the project as %s",
                     name.toUtf8().constData() );
            return target;
        }

        // A NAME COLLISION NEVER OVERWRITES (§B.6). The bytes decide which of
        // the two it is: identical content is a REPEAT drop of the same remote
        // file and reuses the existing copy (a user dragging one loop onto four
        // tracks gets one file, across sessions as well as within one);
        // different content takes the next " (n)".
        if( !wantHash.isEmpty() && fileSha1( target ) == wantHash ) {
            TW_LOGD( "media", "drop: reusing the existing project copy %s",
                     name.toUtf8().constData() );
            return target;
        }
    }

    TW_LOGE( "media", "drop: 999 name collisions under %s — the clip references "
                      "the cache instead",
             mediaDir.toUtf8().constData() );
    return localPath;
}

int collectExternalMedia( SProject *project, QStringList *skippedMissing,
                          QStringList *failed )
{
    if( !project ) return 0;
    const QStringList outside = project->externalMediaPaths();
    if( outside.isEmpty() ) return 0;

    int copied = 0;
    for( const QString &from : outside ) {
        // A MISSING placeholder has no bytes to copy. Named, never counted:
        // this is exactly the case a user meets when they open a travelled
        // project on the machine that does NOT hold the originals, and the one
        // answer that helps is "collect on the machine that has them".
        const SExternFile *ef = project->externFiles().value( from );
        if( ef && ef->isMissing() ) {
            if( skippedMissing ) *skippedMissing << from;
            continue;
        }
        const QString to = materialiseIntoProject(
            from, QFileInfo( from ).fileName(), project );
        // materialiseIntoProject falls back to the SOURCE path on every failure
        // (it logs the reason), so "nothing moved" is how a failure reads here.
        if( to.isEmpty() || to == from ) {
            if( failed ) *failed << from;
            continue;
        }
        if( !project->relocateExternFile( from, to ) ) {
            if( failed ) *failed << from;
            continue;
        }
        ++copied;
    }
    TW_LOGI( "media", "collect: %d file(s) copied into the project, "
                      "%d missing, %d failed",
             copied,
             skippedMissing ? (int) skippedMissing->size() : 0,
             failed ? (int) failed->size() : 0 );
    return copied;
}

void placeWhenLocal( const SMediaRef &ref, SObject *track, offset_t timePos )
{
    if( !ref.isValid() ) {
        ++g_failed;
        TW_LOGE( "media", "drop: an invalid media reference was dropped" );
        return;
    }

    SProject *project = SAppContext::get().getCurrentProject();
    if( !project || !track ) {
        ++g_failed;
        TW_LOGE( "media", "drop: no project or no target track" );
        return;
    }

    SMediaSource *source = SMediaRegistry::instance().source( ref.sourceId );
    if( !source ) {
        ++g_failed;
        TW_LOGE( "media", "drop: no source registered as '%s' — '%s' cannot be "
                          "fetched",
                 ref.sourceId.toUtf8().constData(),
                 ref.path.toUtf8().constData() );
        say( QObject::tr( "No media source '%1' is available." )
                 .arg( ref.sourceId ) );
        return;
    }

    Pending p;
    p.id          = g_nextId++;
    p.ref         = ref;
    p.track       = track;
    p.project     = project;
    p.timePos     = timePos;
    p.displayName = QFileInfo( ref.path ).fileName();
    p.source      = source;

    // ALREADY LOCAL? Then this is synchronous and is exactly one undo step
    // (AC 2) — no pending entry ever exists.
    const QString cached = SMediaCache::instance().lookup( ref );
    if( !cached.isEmpty() ) {
        finishPlacement( p, cached );
        return;
    }

    // Connect ONCE per source. A lambda per drop would fire once per pending
    // placement per completion; the handlers below find their own entry by
    // (source, requestId) instead, which is also what makes AC 6's two
    // concurrent drops of one ref two independent placements.
    static QSet<SMediaSource *> connected;
    if( !connected.contains( source ) ) {
        connected.insert( source );
        QObject::connect( source, &SMediaSource::fetchFinished, source,
                          [ source ]( int id, const QString &path ) {
                              onFetchFinished( source, id, path );
                          } );
        QObject::connect( source, &SMediaSource::requestFailed, source,
                          [ source ]( int id, const QString &message ) {
                              onRequestFailed( source, id, message );
                          } );
        QObject::connect( source, &QObject::destroyed, source,
                          [ source ]( QObject * ) {
                              connected.remove( source );
                          } );
    }

    p.fetchPath = SMediaCache::instance().reserveFetchPath( ref );
    p.requestId = source->fetch( ref.path, p.fetchPath );
    if( p.requestId <= 0 ) {
        ++g_failed;
        TW_LOGE( "media", "drop: the source refused to fetch '%s'",
                 ref.path.toUtf8().constData() );
        say( QObject::tr( "Could not fetch %1." ).arg( p.displayName ) );
        return;
    }
    g_pending.push_back( p );
    say( QObject::tr( "Fetching %1…" ).arg( p.displayName ) );
}

}   // namespace smediadrop
