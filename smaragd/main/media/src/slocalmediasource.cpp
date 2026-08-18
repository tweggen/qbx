#include "app/media/slocalmediasource.h"

#include "smedialocalwalker.h"

#include "tw/core/twlog.h"

#include <QDir>
#include <QFileInfo>
#include <QThreadPool>
#include <QTimer>

namespace smedia {

QThreadPool &mediaThreadPool()
{
    // A PRIVATE pool, never QThreadPool::globalInstance(): a walk blocked
    // inside a single stat() on a dead network mount pins its thread until the
    // OS returns, and it must not be able to starve QtConcurrent or anything
    // else Qt runs on the global pool. Two threads is the whole budget for
    // this module.
    //
    // A function-local static OBJECT, deliberately, rather than a leaked
    // pointer: ~QThreadPool waits for its tasks, so a walk still running at
    // process exit is joined instead of being left to touch Qt internals that
    // static destruction has already taken away.
    static QThreadPool pool;
    static const bool  configured = [] {
        pool.setMaxThreadCount( 2 );
        pool.setObjectName( QStringLiteral( "smedia" ) );
        return true;
    }();
    Q_UNUSED( configured );
    return pool;
}

} // namespace smedia

SLocalMediaSource::SLocalMediaSource( QObject *parent )
    : SMediaSource( parent )
{
}

SLocalMediaSource::~SLocalMediaSource()
{
    // Ask every live walk to wind down. This is a COURTESY, not the safety
    // property: what makes destruction safe is that no task holds a pointer to
    // this object and that Qt drops a queued emission whose receiver has died.
    for( auto it = live_.begin(); it != live_.end(); ++it ) {
        if( it.value() ) it.value()->store( true, std::memory_order_relaxed );
    }
    live_.clear();
}

QString SLocalMediaSource::id() const
{
    return QStringLiteral( "local" );
}

QString SLocalMediaSource::displayName() const
{
    return tr( "Local file system" );
}

int SLocalMediaSource::caps() const
{
    // No NeedsFetch: a local file is already local, and no CanStream — the
    // MVP places files by path, it does not read bytes through a source.
    return CanBrowse | CanSearch;
}

SLocalMediaSource::CancelFlag SLocalMediaSource::beginRequest( int requestId )
{
    CancelFlag flag = std::make_shared<std::atomic_bool>( false );
    live_.insert( requestId, flag );
    return flag;
}

bool SLocalMediaSource::isLive( int requestId ) const
{
    return live_.contains( requestId );
}

void SLocalMediaSource::endRequest( int requestId )
{
    live_.remove( requestId );
}

int SLocalMediaSource::liveRequestCount() const
{
    return live_.size();
}

namespace {

QString normalisedDir( const QString &path )
{
    return QDir::fromNativeSeparators( QDir::cleanPath( path ) );
}

} // namespace

// The worker is created HERE, on the main thread, with NO PARENT (see
// smedialocalwalker.h): parenting it to the source would destroy it under the
// pool thread's feet the moment the source dies.
void SLocalMediaSource::startWalk( const SMediaWalkRequest &request,
                                   CancelFlag flag )
{
    auto *worker = new SMediaWalkWorker();
    connect( worker, &SMediaWalkWorker::batchReady,
             this, &SLocalMediaSource::onWorkerBatch, Qt::QueuedConnection );
    connect( worker, &SMediaWalkWorker::failed,
             this, &SLocalMediaSource::onWorkerFailed, Qt::QueuedConnection );

    auto *task = new SMediaWalkTask( request, worker, std::move( flag ) );
    task->setAutoDelete( true );
    smedia::mediaThreadPool().start( task );
}

int SLocalMediaSource::listDirectory( const QString &dirPath )
{
    const int id = nextRequestId();

    SMediaWalkRequest req;
    req.requestId  = id;
    req.sourceId   = this->id();
    req.rootPath   = normalisedDir( dirPath );
    req.suffixes   = suffixFilter();
    req.recursive  = false;
    req.searchMode = false;

    startWalk( req, beginRequest( id ) );
    return id;
}

int SLocalMediaSource::search( const QString &rootPath, const QString &needle,
                               bool recursive, const QStringList &suffixes )
{
    const int id = nextRequestId();

    SMediaWalkRequest req;
    req.requestId  = id;
    req.sourceId   = this->id();
    req.rootPath   = normalisedDir( rootPath );
    req.needle     = needle;
    // The search request carries its OWN suffix list — a search is a snapshot
    // of the whole query, so a filter change mid-walk cannot make the result
    // disagree with the request that asked for it.
    req.suffixes   = suffixes;
    req.recursive  = recursive;
    req.searchMode = true;

    startWalk( req, beginRequest( id ) );
    return id;
}

int SLocalMediaSource::fetch( const QString &filePath, const QString &destPath )
{
    Q_UNUSED( destPath );
    const int     id   = nextRequestId();
    const QString path = QDir::fromNativeSeparators( filePath );
    beginRequest( id );

    // A local file needs no fetching, but the ABI is async and a caller must
    // never have to special-case which source it is talking to. The answer
    // therefore arrives on the next main-thread turn, exactly as a remote
    // one's would. `this` is the context object, so a source destroyed before
    // the turn simply never sees it.
    QMetaObject::invokeMethod(
        this,
        [this, id, path] {
            if( !isLive( id ) ) return;   // cancelled while queued
            endRequest( id );
            if( QFileInfo::exists( path ) ) {
                emit fetchProgress( id, 1, 1 );
                emit fetchFinished( id, path );
            } else {
                emit requestFailed(
                    id, QStringLiteral( "no such file: %1" ).arg( path ) );
            }
        },
        Qt::QueuedConnection );
    return id;
}

void SLocalMediaSource::cancel( int requestId )
{
    auto it = live_.find( requestId );
    if( it == live_.end() ) return;   // unknown id: a no-op, by contract
    if( it.value() ) it.value()->store( true, std::memory_order_relaxed );
    live_.erase( it );
}

void SLocalMediaSource::onWorkerBatch( int requestId,
                                       const QVector<SMediaEntry> &batch,
                                       bool final, int truncatedCount )
{
    // Supersession and cancellation are BY ID (inv. 2/6): a batch for a
    // request that is no longer live is dropped here, so a cancel cannot be a
    // race the caller has to win.
    if( !isLive( requestId ) ) return;
    if( final ) endRequest( requestId );
    emit entriesReady( requestId, batch, final, truncatedCount );
}

void SLocalMediaSource::onWorkerFailed( int requestId, const QString &message )
{
    if( !isLive( requestId ) ) return;
    endRequest( requestId );
    TW_LOGW( "media", "local source: request %d failed: %s", requestId,
             message.toUtf8().constData() );
    emit requestFailed( requestId, message );
}
