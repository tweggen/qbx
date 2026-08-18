// app/media/swebdavmediasource.h -- the WebDAV SMediaSource (proposal 38 §B.7,
// GATE 4).
//
// Adapts SWebDavClient's PROPFIND/GET transport onto the SMediaSource ABI
// (main/media/CONTRACT.md) exactly the way SLocalMediaSource adapts QDir:
// async, id-tagged, batched, cancel-safe, and honest about what it dropped.
//
// Driven END TO END since gate 5c: SMediaAccountManager (main/shell) registers
// one of these per Nextcloud account, the dock browses and searches it
// (qxa.media_webdav_browse) and a drop out of it places a clip whose rendered
// audio is asserted (qxa.media_webdav_drop). What is still stubbed is the
// SERVER -- main/testkit's SWebDavStub is plain HTTP with no TLS, no redirects
// and no real authentication -- see main/media/CONTRACT.md.

#ifndef SWEBDAVMEDIASOURCE_H
#define SWEBDAVMEDIASOURCE_H

#include "app/media/smediasource.h"
#include "app/media/swebdavclient.h"

#include <QElapsedTimer>
#include <QHash>
#include <QQueue>
#include <QSet>
#include <QUrl>

class SWebDavMediaSource : public SMediaSource
{
    Q_OBJECT

public:
    // `accountId` names this connection ("home", "band-share", ...) and
    // becomes id()'s suffix: "nextcloud:<accountId>". `authorizationHeader`
    // is a full header VALUE (§B.8a) -- "Basic <base64(user:apppassword)>"
    // today, "Bearer <token>" once Login Flow v2 lands -- computed by the
    // CALLER. This class never sees a username or a password.
    SWebDavMediaSource( QString accountId, QUrl baseUrl, QString authorizationHeader,
                        QObject *parent = nullptr );
    ~SWebDavMediaSource() override;

    QString id() const override;
    QString displayName() const override;
    int     caps() const override;

    int  listDirectory( const QString &dirPath ) override;
    int  search( const QString &rootPath, const QString &needle, bool recursive,
                const QStringList &suffixes ) override;
    int  fetch( const QString &filePath, const QString &destPath ) override;
    void cancel( int requestId ) override;

    // How many logical requests are still live (neither completed nor
    // cancelled) -- symmetry with SLocalMediaSource, handy for tests.
    int liveRequestCount() const;

    // Test-only introspection: never used by production code.
    SWebDavClient &client() { return client_; }

private slots:
    void onPropfindFinished( int clientReqId, const QVector<SWebDavClient::Entry> &entries );
    void onPropfindFailed( int clientReqId, int httpStatus, const QString &message );
    void onGetProgress( int clientReqId, qint64 done, qint64 total );
    void onGetFinished( int clientReqId, const QString &localPath );
    void onGetFailed( int clientReqId, int httpStatus, const QString &message );

private:
    // One PROPFIND directly answers a listDirectory(); a search is a
    // breadth-first walk over several. Both share the batching/truncation
    // machinery below (§B.2 inv. 3/4), which is why SearchState is used for
    // both -- a listDirectory() is a search with recursive=false and a
    // one-entry starting queue, which reproduces its actual shape (a single
    // PROPFIND, no needle, no depth limit reached).
    struct SearchState
    {
        int         logicalId = 0;
        QString     needle;          // empty for listDirectory
        QStringList suffixes;
        bool        recursive  = false;
        bool        isListOnly = false;   // true: no suffix filter on dirs is
                                           // different from a search (dirs are
                                           // always listed; see cpp)
        struct Pending { QString path; int depth; };
        QQueue<Pending>   queue;
        QHash<int, int>   inFlight;      // client PROPFIND id -> depth
        int               delivered = 0;
        int               truncated = 0;
        bool              capHit    = false;
        bool              cancelled = false;
        // Distinguishes the ROOT PROPFIND failing (the whole request fails)
        // from a SUBDIRECTORY PROPFIND failing mid-walk (one skipped branch,
        // same distinction the local walker makes for a vanished directory).
        bool              anyCompleted = false;

        QVector<SMediaEntry> pendingBatch;
        QElapsedTimer         sinceFlush;
    };

    SMediaEntry toMediaEntry( const SWebDavClient::Entry &e ) const;

    void appendToSearch( SearchState &st, const SMediaEntry &entry );
    void flushSearch( SearchState &st, bool final );
    void pumpSearch( SearchState &st );
    void finishSearch( int logicalId );

    static QString normalisedPath( const QString &path );

    QString accountId_;

    // logicalId -> state, for a listDirectory()/search() in progress.
    QHash<int, SearchState> searches_;
    // client PROPFIND id -> owning logical search id, so onPropfindFinished
    // knows which SearchState a batch belongs to.
    QHash<int, int> propfindOwner_;

    // client GET id -> logical fetch id (1:1; fetch() never fans out), and
    // the reverse map for an O(1) cancel().
    QHash<int, int> getOwner_;
    QHash<int, int> fetchClientByLogical_;

    // Every logical id that is still live (not yet finished/cancelled).
    // Supersession by id (§B.2 inv. 2): a result for an id no longer in
    // here is dropped, never re-delivered.
    QSet<int> live_;

    // Declared LAST so it is destroyed FIRST: C++ tears members down in
    // reverse declaration order, and client_'s own destructor is what stops
    // a QNetworkReply outliving this source (trap T3, gate 4 AC 7). Its
    // teardown already can't emit a signal back into this class's slots --
    // ~SWebDavClient clears its OWN bookkeeping before calling abort(), so a
    // reentrant finishPropfind()/finishGet() returns before reaching an
    // `emit` -- but declaring it last means that stays true even if that
    // guard is ever weakened, rather than relying on two files agreeing.
    SWebDavClient client_;
};

#endif // SWEBDAVMEDIASOURCE_H
