// app/media/swebdavclient.h -- the WebDAV transport (proposal 38 §B.7).
//
// PROPFIND (Depth: 1) and GET over one QNetworkAccessManager. This class
// knows HTTP and XML; it knows NOTHING about SMediaSource, SMediaRef or the
// dock -- SWebDavMediaSource is the ABI adapter built on top of it, exactly
// as SLocalMediaSource sits on top of QDir.
//
// Four rules from §B.7, non-negotiable:
//
//   * TLS errors are surfaced, never ignored. No call anywhere in this file
//     tells Qt to accept an SSL certificate error, and gate 4 AC 8 greps the
//     whole tree for the exact QNetworkReply method name that would.
//   * A 401 does not retry: one failure signal, once, and nothing here loops.
//   * Every reply is parented to THIS client and aborted in the destructor --
//     a QNetworkReply outliving its owner is a use-after-free with a
//     network-shaped delay on it.
//   * The PROPFIND response body is parsed OFF the GUI thread
//     (QtConcurrent::run + a QFutureWatcher), because a directory of 10 000
//     entries is a megabyte of XML.
//
// `authorizationHeader` is a full HTTP header VALUE ("Basic <base64>" or,
// once Login Flow v2 lands, "Bearer <token>"), computed by the CALLER --
// §B.8a. This class never sees a username or a password, and a future token
// flow is therefore a caller change, not a client change.

#ifndef SWEBDAVCLIENT_H
#define SWEBDAVCLIENT_H

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QString>
#include <QUrl>
#include <QVector>

class QNetworkAccessManager;
class QNetworkReply;
class QSaveFile;

class SWebDavClient : public QObject
{
    Q_OBJECT

public:
    struct Entry
    {
        QString   name;              // display name (<d:displayname>), decoded
        QString   href;              // source-relative, '/'-separated, decoded,
                                      // no leading/trailing slash
        bool      isDir     = false;
        qint64    sizeBytes = -1;    // -1 unknown (a directory, or an entry
                                      // that omitted getcontentlength)
        QDateTime modified;          // invalid = unknown
        QString   etag;              // "" unknown
    };

    explicit SWebDavClient( QUrl baseUrl, QString authorizationHeader,
                            QObject *parent = nullptr );
    ~SWebDavClient() override;

    QUrl baseUrl() const { return baseUrl_; }
    void setAuthorizationHeader( const QString &value ) { authHeader_ = value; }

    // PROPFIND, Depth: 1, on `dirPath` (source-relative, '/'-separated, ""
    // for the root). Returns a request id (> 0), never reused. Results
    // arrive via propfindFinished/propfindFailed on the MAIN thread; the
    // request resource's own entry is never included (§B.2 inv., AC 2).
    int propfind( const QString &dirPath );

    // GET `filePath` (source-relative), streamed to `destPath` through a
    // QSaveFile: write-to-temp-then-atomic-rename, so a cancelled transfer or
    // one interrupted mid-body leaves no partial file and no `.tmp` behind
    // (gate 4 ACs 4, 5).
    int get( const QString &filePath, const QString &destPath );

    // Advisory (§B.2 inv. 6): aborts the underlying reply if one is still in
    // flight. A PROPFIND whose network round trip already finished and whose
    // XML parse is in flight on the concurrent pool is NOT interrupted -- its
    // result is still delivered, tagged with this same id, and it is the
    // caller's job (SWebDavMediaSource) to drop it by id, the same
    // supersession rule the local provider uses.
    void cancel( int requestId );

    int liveRequestCount() const { return requests_.size(); }

signals:
    void propfindFinished( int requestId, const QVector<SWebDavClient::Entry> &entries );
    void propfindFailed( int requestId, int httpStatus, const QString &message );

    // `total` is -1 when the server did not send Content-Length.
    void getProgress( int requestId, qint64 done, qint64 total );
    void getFinished( int requestId, const QString &localPath );
    void getFailed( int requestId, int httpStatus, const QString &message );

private:
    enum class Kind { Propfind, Get };

    struct RequestState
    {
        Kind           kind  = Kind::Propfind;
        QNetworkReply *reply = nullptr;   // parented to `this`
        QSaveFile     *file  = nullptr;   // Get only; owned by this state
        QString        destPath;
    };

    QUrl    urlFor( const QString &relPath ) const;
    void    finishPropfind( int requestId );
    void    finishGet( int requestId );

    QNetworkAccessManager   *nam_;
    QUrl                     baseUrl_;
    QString                  basePathDecoded_;   // baseUrl_.path(), decoded, trailing '/'
    QString                  authHeader_;
    int                      nextId_ = 0;
    QHash<int, RequestState> requests_;
};

#endif // SWEBDAVCLIENT_H
