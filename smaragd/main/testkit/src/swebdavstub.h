// app/testkit/swebdavstub.h -- an in-process WebDAV server, for GATE 4's
// unit test today and gate 5c's `media-webdav-stub` verb later.
//
// Proposal 38 §C GATE 4. Lives in main/testkit (NOT main/media/tests)
// because a later gate starts one inside smaragd.exe, which links testkit;
// gate 4's own unit test (main/media/tests/webdav_source_test.cpp) links it
// from here too. Speaks just enough HTTP/1.1 to answer PROPFIND (Depth: 1)
// with a canned multistatus body built from a table, and GET with the bytes
// of a registered fixture -- plus the failure modes gate 4 needs to gate:
// 401 / 404 / 500, a mid-body close, and a stall.
//
// Binds 127.0.0.1:0 (an OS-assigned port) -- never a fixed port, which is
// what keeps this safe under `ctest -j4` (proposal 38 §C GATE 4, trap: "four
// concurrent cases cannot collide over one port").
//
// Plain HTTP, no TLS: TLS is Qt's code, not ours, and a self-signed
// certificate in the repo would be a liability (§C GATE 4). TLS-error
// surfacing is gated manually against a real server.

#ifndef SWEBDAVSTUB_H
#define SWEBDAVSTUB_H

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QUrl>
#include <QVector>

class QTcpServer;
class QTcpSocket;

struct SWebDavStubEntry
{
    QString   name;                 // decoded display name, e.g. "kick#1.wav"
    bool      isDir     = false;
    qint64    sizeBytes = -1;       // -1: omit <getcontentlength> (AC 2's
                                     // "missing optional property" case)
    QDateTime modified;             // invalid: omit <getlastmodified>
    QString   etag;                 // empty: omit <getetag>
    QByteArray content;             // GET body, for a file entry
};

class SWebDavStub : public QObject
{
    Q_OBJECT

public:
    enum class Fault { None, Status401, Status404, Status500, CloseMidBody, Stall };

    explicit SWebDavStub( QObject *parent = nullptr );
    ~SWebDavStub() override;

    // Binds 127.0.0.1:0. Returns false (and logs) on failure. Idempotent
    // while already listening.
    bool start();
    void stop();
    bool isListening() const;

    quint16 port() const;
    // "http://127.0.0.1:<port>/dav/" -- the fixed base path every dir/file
    // path in setDirectory()/GET is resolved under.
    QUrl baseUrl() const;

    // `dirPath` is '/'-separated, no leading/trailing slash ("" is the
    // root). Registers this directory's listing AND, for every non-dir entry
    // that carries `content`, that file's GET body at `dirPath/entry.name`
    // -- one call wires up both PROPFIND and GET for a fixture.
    void setDirectory( const QString &dirPath, const QVector<SWebDavStubEntry> &entries );
    void clearDirectories();

    // Fault injection, by the exact request path (decoded, '/'-separated, no
    // leading slash) a PROPFIND or GET would use.
    void setFault( const QString &path, Fault fault );
    void clearFaults();

    // The <d:...> / <D:...> / unprefixed namespace a server spells its
    // responses with. Real servers vary (AC 2's "namespace prefixes... d:,
    // D:, none"); empty string means an unprefixed default xmlns. Default
    // "d".
    void setNamespacePrefix( const QString &prefix );

    // An artificial delay (ms) before a PROPFIND/GET starts answering, and
    // the number of GET body chunks a fixture is split into. Both widen the
    // window in which several requests are genuinely concurrent / a
    // download genuinely straddles several downloadProgress deliveries.
    // Default: 0 delay, 1 chunk (i.e. off).
    void setResponseDelayMs( int ms );
    void setGetChunkCount( int chunks );

    // Counters, reset via resetCounters(). requestCount() counts a path
    // that also faulted -- the point of AC 4's "the stub counts requests"
    // is exactly to prove a 401 was asked for ONCE.
    int totalRequests() const { return totalRequests_; }
    int requestCount( const QString &path ) const;
    int peakConcurrentRequests() const { return peakConcurrent_; }
    void resetCounters();

private:
    struct Conn
    {
        QTcpSocket *socket = nullptr;
        QByteArray  inBuffer;
        // GET response streaming state.
        QByteArray  pendingBody;
        int         pendingOffset      = 0;
        int         pendingChunkSize   = 0;
        bool        pendingCloseMidBody = false;
    };

    void onNewConnection();
    void onReadyRead( Conn *conn );
    void onDisconnected( Conn *conn );

    void handleRequest( Conn *conn, const QString &method, const QString &rawPath );
    void writeResponseFor( Conn *conn, const QString &method, const QString &path, Fault fault );
    void respondPropfind( Conn *conn, const QString &path );
    void startGetResponse( Conn *conn, const QString &path, bool closeMidBody );
    void sendNextChunk( Conn *conn );
    void writeSimpleStatus( Conn *conn, int code, const QString &reason );

    QString normalisedRequestPath( const QString &rawPath ) const;
    QString hrefFor( const QString &dirPath, const QString &name, bool isDir ) const;
    QByteArray buildMultistatus( const QString &dirPath,
                                const QVector<SWebDavStubEntry> &entries ) const;
    QString responseFragment( const QString &prefix, const QString &href, const QString &name,
                              bool isDir, qint64 sizeBytes, const QDateTime &modified,
                              const QString &etag ) const;

    Fault faultFor( const QString &path ) const;

    QTcpServer *server_;
    QSet<Conn *> liveConns_;

    QHash<QString, QVector<SWebDavStubEntry>> dirs_;    // normalised dirPath -> entries
    QHash<QString, QByteArray>                files_;   // normalised filePath -> content
    QHash<QString, Fault>                     faults_;  // normalised path -> fault

    QString namespacePrefix_ = QStringLiteral( "d" );
    int     responseDelayMs_ = 0;
    int     getChunkCount_   = 1;

    int totalRequests_  = 0;
    int activeCount_    = 0;
    int peakConcurrent_ = 0;
    QHash<QString, int> countByPath_;

    static const QString kBasePrefix;   // "/dav/"
};

#endif // SWEBDAVSTUB_H
