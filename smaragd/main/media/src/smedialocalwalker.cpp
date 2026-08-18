#include "smedialocalwalker.h"

#include "app/media/slocalmediasource.h"
#include "app/media/smediatypes.h"

#include "tw/core/twlog.h"

#include <QDir>
#include <QFileInfo>
#include <QQueue>

// ---------------------------------------------------------------- worker ---

SMediaWalkWorker::SMediaWalkWorker( QObject *parent )
    : QObject( parent )
{
}

void SMediaWalkWorker::postBatch( int requestId, const QVector<SMediaEntry> &batch,
                                  bool final, int truncatedCount )
{
    emit batchReady( requestId, batch, final, truncatedCount );
}

void SMediaWalkWorker::postFailed( int requestId, const QString &message )
{
    emit failed( requestId, message );
}

// ------------------------------------------------------------------ task ---

SMediaWalkTask::SMediaWalkTask( SMediaWalkRequest request,
                                SMediaWalkWorker *worker,
                                std::shared_ptr<std::atomic_bool> cancelled )
    : req_( std::move( request ) )
    , worker_( worker )
    , cancelled_( std::move( cancelled ) )
{
}

SMediaWalkTask::~SMediaWalkTask()
{
    // The worker outlives the task by exactly one deferred-delete event, and
    // it is deleted on ITS OWN thread (the main one). Deleting it here would
    // destroy a QObject from a pool thread.
    if( worker_ ) worker_->deleteLater();
}

bool SMediaWalkTask::cancelled() const
{
    return cancelled_ && cancelled_->load( std::memory_order_relaxed );
}

void SMediaWalkTask::append( const SMediaEntry &entry )
{
    if( delivered_ + pending_.size() >= smedia::kMaxSearchEntries ) {
        // Inv. 4: a bound is ANNOUNCED. truncated_ is a LOWER BOUND on what
        // was not delivered — the walk stops here, so what lies beyond is by
        // definition uncounted.
        ++truncated_;
        capHit_ = true;
        return;
    }
    pending_.append( entry );
    if( pending_.size() >= smedia::kBatchEntries
        || ( sinceFlush_.isValid()
             && sinceFlush_.elapsed() >= smedia::kBatchMs ) ) {
        flush( false );
    }
}

void SMediaWalkTask::flush( bool final )
{
    if( !final && pending_.isEmpty() ) return;
    delivered_ += pending_.size();
    worker_->postBatch( req_.requestId, pending_, final,
                        final ? truncated_ : 0 );
    pending_.clear();
    sinceFlush_.restart();
}

bool SMediaWalkTask::listOne( const QString &dirPath,
                              QVector<SMediaEntry> *dirsOut,
                              QVector<SMediaEntry> *filesOut,
                              QString *errorOut ) const
{
    QDir dir( dirPath );
    if( !dir.exists() ) {
        if( errorOut ) {
            *errorOut = QStringLiteral( "no such directory: %1" ).arg( dirPath );
        }
        return false;
    }

    // DirsFirst | Name is the ordering AC 2 names: directories before files,
    // each sorted by name. IgnoreCase keeps "Kick.wav" beside "kick.wav"
    // rather than in a separate ASCII block.
    const QFileInfoList infos =
        dir.entryInfoList( QDir::AllEntries | QDir::NoDotAndDotDot,
                           QDir::DirsFirst | QDir::Name | QDir::IgnoreCase );

    for( const QFileInfo &info : infos ) {
        SMediaEntry e;
        e.name           = info.fileName();
        e.ref.sourceId   = req_.sourceId;
        e.ref.path       = QDir::fromNativeSeparators( info.absoluteFilePath() );
        e.isDir          = info.isDir();
        // A directory's size() is platform junk (0 here, a block count there),
        // so it stays the documented "unknown". -1, never a number that
        // differs between Windows and Linux in an exact-count gate.
        e.sizeBytes      = e.isDir ? -1 : info.size();
        e.modified       = info.lastModified();
        if( e.isDir ) {
            if( dirsOut ) dirsOut->append( e );
        } else if( filesOut ) {
            filesOut->append( e );
        }
    }
    return true;
}

void SMediaWalkTask::runList()
{
    QVector<SMediaEntry> dirs, files;
    QString              error;
    if( !listOne( req_.rootPath, &dirs, &files, &error ) ) {
        worker_->postFailed( req_.requestId, error );
        return;
    }

    // Directories are ALWAYS listed — the filter is about files, and a user
    // must be able to navigate past a filter into a folder it hides.
    for( const SMediaEntry &e : dirs ) {
        if( cancelled() ) return;
        append( e );
        if( capHit_ ) break;
    }
    for( const SMediaEntry &e : files ) {
        if( cancelled() ) return;
        if( !smedia::suffixAccepted( e.name, req_.suffixes ) ) continue;
        append( e );
        if( capHit_ ) break;
    }
    if( cancelled() ) return;
    flush( true );
}

void SMediaWalkTask::runSearch()
{
    // Breadth-first, so the shallow hits — the ones a user is most likely to
    // want — stream first. Search reports FILES only: a directory is a
    // container to walk, not a result, and a flat result list of folders is
    // not what the search box means.
    struct Pending { QString path; int depth; };
    QQueue<Pending> queue;
    queue.enqueue( { req_.rootPath, 0 } );

    bool first = true;
    while( !queue.isEmpty() ) {
        if( cancelled() ) return;
        const Pending here = queue.dequeue();

        QVector<SMediaEntry> dirs, files;
        QString              error;
        if( !listOne( here.path, &dirs, &files, &error ) ) {
            if( first ) {
                // The ROOT not existing is a failed request; a subdirectory
                // that vanished mid-walk is not — it is one skipped branch.
                worker_->postFailed( req_.requestId, error );
                return;
            }
            ++truncated_;
            continue;
        }
        first = false;

        for( const SMediaEntry &e : files ) {
            if( cancelled() ) return;
            if( !smedia::suffixAccepted( e.name, req_.suffixes ) ) continue;
            if( !req_.needle.isEmpty()
                && !e.name.contains( req_.needle, Qt::CaseInsensitive ) ) {
                continue;
            }
            append( e );
            if( capHit_ ) break;
        }
        if( capHit_ ) break;

        if( !req_.recursive ) break;   // depth 1 and no further

        if( here.depth + 1 > smedia::kMaxSearchDepth ) {
            // One bound per branch not taken. A lower bound, announced.
            truncated_ += dirs.size();
            continue;
        }
        for( const SMediaEntry &d : dirs ) {
            // Symlinks are NOT followed (§B.4): it bounds a loop without
            // relying on the depth cap, and it makes an exact-count gate a
            // fact about the fixture rather than about the walker's mood.
            if( QFileInfo( d.ref.path ).isSymLink() ) continue;
            queue.enqueue( { d.ref.path, here.depth + 1 } );
        }
    }

    if( cancelled() ) return;
    if( truncated_ > 0 ) {
        TW_LOGW( "media",
                 "search bounded: request %d delivered %d entries and dropped "
                 "at least %d (caps: %d entries, depth %d)",
                 req_.requestId, delivered_ + pending_.size(), truncated_,
                 smedia::kMaxSearchEntries, smedia::kMaxSearchDepth );
    }
    flush( true );
}

void SMediaWalkTask::run()
{
    sinceFlush_.start();
    if( cancelled() ) return;   // cancelled before the pool ever got to it
    if( req_.searchMode ) {
        runSearch();
    } else {
        runList();
    }
}
