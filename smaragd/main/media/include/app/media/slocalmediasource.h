// app/media/slocalmediasource.h — the local file-system provider.
//
// Proposal 38 §B.2. Everything here is async and id-tagged, walked on a
// PRIVATE bounded QThreadPool (never QThreadPool::globalInstance(), and never
// a raw OS thread -- see CONTRACT.md inv. 1), handed back to the main thread
// through a worker QObject's SIGNAL over a QUEUED connection.
//
// Note on the wording: gate 1 AC 9 is a literal grep for the raw-thread
// spelling over main/media/, so no SOURCE file here may contain it, not even
// in a comment. CONTRACT.md does, because AC 10 requires it to quote §B.2's
// invariants verbatim.
//
// THE HAND-BACK IS THE ONE THING NOT TO "SIMPLIFY". The obvious version — the
// pool thread calling QMetaObject::invokeMethod on the source pointer — is a
// use-after-free: the pool thread dereferences the source to post the event,
// and a source can be deleted at any moment. A QPointer does not fix it
// either; a QPointer is only safe to read on the thread that owns the object.
// A queued emission whose RECEIVER dies is dropped by Qt under its own
// connection mutex, which is what makes the safe version safe.

#ifndef SLOCALMEDIASOURCE_H
#define SLOCALMEDIASOURCE_H

#include "app/media/smediasource.h"

#include <atomic>
#include <memory>

#include <QHash>
#include <QString>

class QThreadPool;
struct SMediaWalkRequest;

namespace smedia {

// The walk's bounds (§B.2 inv. 4). Hitting one is ANNOUNCED through
// `truncatedCount`, never silent.
constexpr int kMaxSearchEntries = 5000;
constexpr int kMaxSearchDepth   = 12;

// Batching (§B.2 inv. 3): flush every kBatchEntries entries or kBatchMs
// milliseconds, whichever comes first, so a recursive walk shows its first
// hits immediately.
constexpr int kBatchEntries = 200;
constexpr int kBatchMs      = 100;

// The module's PRIVATE pool. Two threads, dedicated to media walks, so a
// blocked walk cannot starve QtConcurrent or anything else Qt runs on the
// global pool.
//
// NAMED LIMITATION: cancel() is checked BETWEEN entries, so a walker blocked
// inside a single stat() — a dead network mount, a spun-down drive — never
// sees it and pins its thread until the OS returns. That cannot be designed
// away without an out-of-process walker.
QThreadPool &mediaThreadPool();

} // namespace smedia

class SLocalMediaSource : public SMediaSource
{
    Q_OBJECT

public:
    explicit SLocalMediaSource( QObject *parent = nullptr );
    ~SLocalMediaSource() override;

    QString id() const override;
    QString displayName() const override;
    int     caps() const override;

    int  listDirectory( const QString &dirPath ) override;
    int  search( const QString &rootPath, const QString &needle,
                 bool recursive, const QStringList &suffixes ) override;
    // A local file is already local: fetch() completes on the next main-thread
    // turn with the path it was given, or fails if the file is not there. It
    // never copies to destPath — a local source has nothing to fetch, and
    // NeedsFetch is deliberately absent from caps().
    int  fetch( const QString &filePath, const QString &destPath ) override;
    void cancel( int requestId ) override;

    // How many requests are still live (neither completed nor cancelled).
    int liveRequestCount() const;

private slots:
    // The ONLY entry point a pool thread's result takes. Both are connected
    // Qt::QueuedConnection from the per-request worker.
    void onWorkerBatch( int requestId, const QVector<SMediaEntry> &batch,
                        bool final, int truncatedCount );
    void onWorkerFailed( int requestId, const QString &message );

private:
    using CancelFlag = std::shared_ptr<std::atomic_bool>;

    // Turns a request into a running walk: creates the worker QObject on THIS
    // thread with no parent, connects it queued, and hands the task to the
    // private pool. The one place that shape is spelled out.
    void       startWalk( const SMediaWalkRequest &request, CancelFlag flag );
    CancelFlag beginRequest( int requestId );
    bool       isLive( int requestId ) const;
    void       endRequest( int requestId );

    QHash<int, CancelFlag> live_;
};

#endif // SLOCALMEDIASOURCE_H
