// main/media/src/smedialocalwalker.h — PRIVATE to app/media.
//
// The off-thread half of SLocalMediaSource: a plain QRunnable that walks the
// file system, plus the worker QObject that is the ONLY way its results reach
// the main thread (proposal 38 §B.2 inv. 1, trap T16).
//
// Read this before changing the hand-back:
//
//   * The worker is created on the MAIN thread, has NO PARENT, and is owned by
//     the task. Parenting it to the source would destroy it under the pool
//     thread's feet the moment the source dies — the exact use-after-free the
//     invariant exists to prevent.
//   * The task holds NO pointer to the source. It cannot, even in principle,
//     touch a destroyed one. Cancellation reaches it through a shared
//     std::atomic_bool that outlives the source.
//   * Results travel as a SIGNAL over a Qt::QueuedConnection, never as
//     QMetaObject::invokeMethod on a raw source pointer: Qt tears connections
//     down under its own mutex when the receiver dies, so a queued emission to
//     a dead receiver is DROPPED, not crashed. A QPointer would not help — a
//     QPointer is only safe to read on the thread that owns the object.

#ifndef SMEDIALOCALWALKER_H
#define SMEDIALOCALWALKER_H

#include "app/media/smediaref.h"

#include <atomic>
#include <memory>

#include <QElapsedTimer>
#include <QObject>
#include <QRunnable>
#include <QString>
#include <QStringList>
#include <QVector>

// The one object a pool thread is allowed to speak through.
class SMediaWalkWorker : public QObject
{
    Q_OBJECT

public:
    explicit SMediaWalkWorker( QObject *parent = nullptr );

    // Called from the POOL THREAD. Both emit; the connections are queued.
    void postBatch( int requestId, const QVector<SMediaEntry> &batch,
                    bool final, int truncatedCount );
    void postFailed( int requestId, const QString &message );

signals:
    void batchReady( int requestId, const QVector<SMediaEntry> &batch,
                     bool final, int truncatedCount );
    void failed( int requestId, const QString &message );
};

struct SMediaWalkRequest
{
    int         requestId  = 0;
    QString     sourceId;
    QString     rootPath;    // absolute, '/'-separated
    QString     needle;      // empty = match every name
    QStringList suffixes;    // empty = no suffix filter
    bool        recursive  = false;
    bool        searchMode  = false;   // false = listDirectory
};

class SMediaWalkTask : public QRunnable
{
public:
    SMediaWalkTask( SMediaWalkRequest request, SMediaWalkWorker *worker,
                    std::shared_ptr<std::atomic_bool> cancelled );
    ~SMediaWalkTask() override;

    void run() override;

private:
    bool cancelled() const;
    void append( const SMediaEntry &entry );
    void flush( bool final );
    bool listOne( const QString &dirPath, QVector<SMediaEntry> *dirsOut,
                  QVector<SMediaEntry> *filesOut, QString *errorOut ) const;

    void runList();
    void runSearch();

    SMediaWalkRequest                 req_;
    SMediaWalkWorker                 *worker_ = nullptr;   // owned
    std::shared_ptr<std::atomic_bool> cancelled_;

    QVector<SMediaEntry> pending_;
    QElapsedTimer        sinceFlush_;
    int                  delivered_ = 0;
    int                  truncated_ = 0;
    bool                 capHit_    = false;
};

#endif // SMEDIALOCALWALKER_H
