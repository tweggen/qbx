#include "swebdavstub.h"

#include "tw/core/twlog.h"

#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>

const QString SWebDavStub::kBasePrefix = QStringLiteral( "/dav/" );

namespace {
// Spaced wide enough that the CLIENT's downloadProgress reliably fires once
// per chunk rather than coalescing several loopback writes into one
// QNetworkReply::readyRead (measured: 5 ms let 8 chunks collapse into a
// single delivery often enough to make gate 4 AC 6 flaky).
constexpr int kChunkDelayMs = 25;
} // namespace

SWebDavStub::SWebDavStub( QObject *parent )
    : QObject( parent )
    , server_( new QTcpServer( this ) )
{
    connect( server_, &QTcpServer::newConnection, this, &SWebDavStub::onNewConnection );
}

SWebDavStub::~SWebDavStub()
{
    stop();
}

bool SWebDavStub::start()
{
    if( server_->isListening() ) return true;
    // 127.0.0.1:0 -- an OS-assigned port, never a fixed one, so four
    // concurrent `ctest -j4` cases cannot collide over it.
    if( !server_->listen( QHostAddress::LocalHost, 0 ) ) {
        TW_LOGW( "media", "webdav stub: listen() failed: %s",
                 server_->errorString().toUtf8().constData() );
        return false;
    }
    return true;
}

void SWebDavStub::stop()
{
    server_->close();
    const QSet<Conn *> conns = liveConns_;
    liveConns_.clear();
    for( Conn *c : conns ) {
        if( c->socket ) c->socket->deleteLater();
        delete c;
    }
}

bool SWebDavStub::isListening() const
{
    return server_->isListening();
}

quint16 SWebDavStub::port() const
{
    return server_->serverPort();
}

QUrl SWebDavStub::baseUrl() const
{
    QUrl u;
    u.setScheme( QStringLiteral( "http" ) );
    u.setHost( QStringLiteral( "127.0.0.1" ) );
    u.setPort( port() );
    u.setPath( kBasePrefix );
    return u;
}

void SWebDavStub::setDirectory( const QString &dirPath, const QVector<SWebDavStubEntry> &entries )
{
    QString d = dirPath;
    while( d.startsWith( QLatin1Char( '/' ) ) ) d.remove( 0, 1 );
    while( d.endsWith( QLatin1Char( '/' ) ) ) d.chop( 1 );
    dirs_.insert( d, entries );

    for( const SWebDavStubEntry &e : entries ) {
        if( e.isDir ) continue;
        const QString filePath = d.isEmpty() ? e.name : d + QLatin1Char( '/' ) + e.name;
        files_.insert( filePath, e.content );
    }
}

void SWebDavStub::clearDirectories()
{
    dirs_.clear();
    files_.clear();
}

void SWebDavStub::setFault( const QString &path, Fault fault )
{
    QString p = path;
    while( p.startsWith( QLatin1Char( '/' ) ) ) p.remove( 0, 1 );
    while( p.endsWith( QLatin1Char( '/' ) ) ) p.chop( 1 );
    if( fault == Fault::None ) faults_.remove( p );
    else faults_.insert( p, fault );
}

void SWebDavStub::clearFaults()
{
    faults_.clear();
}

void SWebDavStub::setNamespacePrefix( const QString &prefix )
{
    namespacePrefix_ = prefix;
}

void SWebDavStub::setExpectedAuthorization( const QString &headerValue )
{
    expectedAuth_ = headerValue;
}

void SWebDavStub::setResponseDelayMs( int ms )
{
    responseDelayMs_ = qMax( 0, ms );
}

void SWebDavStub::setGetChunkCount( int chunks )
{
    getChunkCount_ = qMax( 1, chunks );
}

int SWebDavStub::requestCount( const QString &path ) const
{
    QString p = path;
    while( p.startsWith( QLatin1Char( '/' ) ) ) p.remove( 0, 1 );
    while( p.endsWith( QLatin1Char( '/' ) ) ) p.chop( 1 );
    return countByPath_.value( p, 0 );
}

void SWebDavStub::resetCounters()
{
    totalRequests_ = 0;
    activeCount_   = 0;
    peakConcurrent_ = 0;
    countByPath_.clear();
}

SWebDavStub::Fault SWebDavStub::faultFor( const QString &path ) const
{
    return faults_.value( path, Fault::None );
}

// --------------------------------------------------------------- accept ---

void SWebDavStub::onNewConnection()
{
    while( server_->hasPendingConnections() ) {
        QTcpSocket *sock = server_->nextPendingConnection();
        auto       *conn = new Conn();
        conn->socket = sock;
        liveConns_.insert( conn );

        connect( sock, &QTcpSocket::readyRead, this, [this, conn] { onReadyRead( conn ); } );
        connect( sock, &QTcpSocket::disconnected, this,
                 [this, conn] { onDisconnected( conn ); } );
    }
}

void SWebDavStub::onDisconnected( Conn *conn )
{
    if( !liveConns_.remove( conn ) ) return;   // already cleaned up
    conn->socket->deleteLater();
    delete conn;
}

// ------------------------------------------------------------- request ---

void SWebDavStub::onReadyRead( Conn *conn )
{
    if( !conn->socket ) return;
    conn->inBuffer.append( conn->socket->readAll() );

    const int headerEnd = conn->inBuffer.indexOf( "\r\n\r\n" );
    if( headerEnd < 0 ) return;   // headers not complete yet

    const QByteArray       headerBlock = conn->inBuffer.left( headerEnd );
    const QList<QByteArray> lines      = headerBlock.split( '\n' );
    if( lines.isEmpty() ) return;

    const QString    requestLine = QString::fromLatin1( lines.at( 0 ) ).trimmed();
    const QStringList parts      = requestLine.split( QLatin1Char( ' ' ), Qt::SkipEmptyParts );
    if( parts.size() < 2 ) {
        conn->socket->abort();
        return;
    }
    const QString method  = parts.at( 0 ).toUpper();
    const QString rawPath = parts.at( 1 );

    qint64  contentLength = 0;
    QString authorization;              // "" also means "the client sent none"
    for( int i = 1; i < lines.size(); ++i ) {
        const QString line = QString::fromLatin1( lines.at( i ) ).trimmed();
        const int     colon = line.indexOf( QLatin1Char( ':' ) );
        if( colon < 0 ) continue;
        const QString key = line.left( colon ).trimmed().toLower();
        if( key == QLatin1String( "content-length" ) ) {
            contentLength = line.mid( colon + 1 ).trimmed().toLongLong();
        } else if( key == QLatin1String( "authorization" ) ) {
            // The VALUE, verbatim apart from the surrounding whitespace HTTP
            // allows. Never logged, never echoed into a response body: it
            // carries base64(user:password) and §B.8 rule 2 forbids a secret
            // reaching a log even from the test half of the wire.
            authorization = line.mid( colon + 1 ).trimmed();
        }
    }

    const int bodyStart = headerEnd + 4;
    if( conn->inBuffer.size() - bodyStart < contentLength ) return;   // body not complete yet

    // The PROPFIND request body (the requested prop list) is not consulted --
    // the stub always answers the fixed superset of properties gate 4's ACs
    // exercise. Drop the consumed bytes so a reused connection (unused by
    // our tests, but harmless to support) starts clean.
    conn->inBuffer.remove( 0, bodyStart + contentLength );

    handleRequest( conn, method, rawPath, authorization );
}

QString SWebDavStub::normalisedRequestPath( const QString &rawPath ) const
{
    QString p = rawPath;
    const int q = p.indexOf( QLatin1Char( '?' ) );
    if( q >= 0 ) p = p.left( q );
    p = QUrl::fromPercentEncoding( p.toUtf8() );
    if( p.startsWith( kBasePrefix ) ) p = p.mid( kBasePrefix.length() );
    while( p.startsWith( QLatin1Char( '/' ) ) ) p.remove( 0, 1 );
    while( p.endsWith( QLatin1Char( '/' ) ) ) p.chop( 1 );
    return p;
}

void SWebDavStub::handleRequest( Conn *conn, const QString &method, const QString &rawPath,
                                 const QString &authorization )
{
    const QString path = normalisedRequestPath( rawPath );

    ++totalRequests_;
    ++countByPath_[path];
    ++activeCount_;
    peakConcurrent_ = qMax( peakConcurrent_, activeCount_ );

    // The credential is checked BEFORE the injected fault, because that is the
    // order a real server works in: a request that cannot authenticate is
    // rejected before anything decides what it was asking for. The request is
    // still COUNTED above -- gate 4's "the stub counts requests" exists to
    // prove a 401 was asked for exactly once.
    Fault fault = faultFor( path );
    if( !expectedAuth_.isEmpty() && authorization != expectedAuth_ ) fault = Fault::Status401;
    const int   delay = responseDelayMs_;

    if( delay > 0 ) {
        QTimer::singleShot( delay, this, [this, conn, method, path, fault] {
            if( !liveConns_.contains( conn ) ) return;   // client gave up already
            --activeCount_;
            writeResponseFor( conn, method, path, fault );
        } );
    } else {
        --activeCount_;
        writeResponseFor( conn, method, path, fault );
    }
}

void SWebDavStub::writeResponseFor( Conn *conn, const QString &method, const QString &path,
                                    Fault fault )
{
    if( fault == Fault::Stall ) return;   // deliberately never responds

    if( fault == Fault::Status401 ) {
        writeSimpleStatus( conn, 401, QStringLiteral( "Unauthorized" ) );
        return;
    }
    if( fault == Fault::Status404 ) {
        writeSimpleStatus( conn, 404, QStringLiteral( "Not Found" ) );
        return;
    }
    if( fault == Fault::Status500 ) {
        writeSimpleStatus( conn, 500, QStringLiteral( "Internal Server Error" ) );
        return;
    }

    if( method == QLatin1String( "PROPFIND" ) ) {
        respondPropfind( conn, path );
    } else if( method == QLatin1String( "GET" ) ) {
        startGetResponse( conn, path, fault == Fault::CloseMidBody );
    } else {
        writeSimpleStatus( conn, 501, QStringLiteral( "Not Implemented" ) );
    }
}

void SWebDavStub::writeSimpleStatus( Conn *conn, int code, const QString &reason )
{
    if( !conn->socket ) return;
    const QByteArray header =
        QStringLiteral( "HTTP/1.1 %1 %2\r\nContent-Length: 0\r\nConnection: close\r\n\r\n" )
            .arg( code )
            .arg( reason )
            .toUtf8();
    conn->socket->write( header );
    conn->socket->disconnectFromHost();
}

void SWebDavStub::respondPropfind( Conn *conn, const QString &path )
{
    if( !conn->socket ) return;
    const auto it = dirs_.constFind( path );
    if( it == dirs_.constEnd() ) {
        writeSimpleStatus( conn, 404, QStringLiteral( "Not Found" ) );
        return;
    }

    const QByteArray body = buildMultistatus( path, it.value() );
    const QByteArray header =
        QStringLiteral( "HTTP/1.1 207 Multi-Status\r\n"
                        "Content-Type: application/xml; charset=utf-8\r\n"
                        "Content-Length: %1\r\n"
                        "Connection: close\r\n\r\n" )
            .arg( body.size() )
            .toUtf8();
    conn->socket->write( header );
    conn->socket->write( body );
    conn->socket->disconnectFromHost();
}

void SWebDavStub::startGetResponse( Conn *conn, const QString &path, bool closeMidBody )
{
    if( !conn->socket ) return;
    const auto it = files_.constFind( path );
    if( it == files_.constEnd() ) {
        writeSimpleStatus( conn, 404, QStringLiteral( "Not Found" ) );
        return;
    }

    const QByteArray content = it.value();
    const QByteArray header =
        QStringLiteral( "HTTP/1.1 200 OK\r\n"
                        "Content-Type: application/octet-stream\r\n"
                        "Content-Length: %1\r\n"
                        "Connection: close\r\n\r\n" )
            .arg( content.size() )
            .toUtf8();
    conn->socket->write( header );

    conn->pendingBody         = content;
    conn->pendingOffset       = 0;
    conn->pendingChunkSize    = qMax( 1, content.size() / getChunkCount_ );
    conn->pendingCloseMidBody = closeMidBody;
    sendNextChunk( conn );
}

void SWebDavStub::sendNextChunk( Conn *conn )
{
    if( !liveConns_.contains( conn ) || !conn->socket ) return;

    if( conn->pendingCloseMidBody
        && conn->pendingOffset >= conn->pendingBody.size() / 2 ) {
        // A connection closed mid-body: the client sees fewer bytes than the
        // Content-Length it was promised (gate 4 AC 4/5). abort() rather
        // than a graceful close -- a graceful FIN after a short write is
        // exactly what a real server does when it dies mid-response.
        conn->socket->flush();
        conn->socket->abort();
        return;
    }

    if( conn->pendingOffset >= conn->pendingBody.size() ) {
        conn->socket->disconnectFromHost();
        return;
    }

    const int n = qMin( conn->pendingChunkSize, conn->pendingBody.size() - conn->pendingOffset );
    conn->socket->write( conn->pendingBody.constData() + conn->pendingOffset, n );
    conn->pendingOffset += n;

    QTimer::singleShot( kChunkDelayMs, this, [this, conn] { sendNextChunk( conn ); } );
}

// ------------------------------------------------------------------ XML ---

QString SWebDavStub::hrefFor( const QString &dirPath, const QString &name, bool isDir ) const
{
    QStringList segs = dirPath.split( QLatin1Char( '/' ), Qt::SkipEmptyParts );
    if( !name.isEmpty() ) segs << name;

    QStringList encoded;
    encoded.reserve( segs.size() );
    for( const QString &s : segs ) {
        encoded.append( QString::fromUtf8( QUrl::toPercentEncoding( s ) ) );
    }

    QString path = kBasePrefix;
    if( !encoded.isEmpty() ) path += encoded.join( QLatin1Char( '/' ) );
    if( isDir && !path.endsWith( QLatin1Char( '/' ) ) ) path += QLatin1Char( '/' );
    return path;
}

QString SWebDavStub::responseFragment( const QString &prefix, const QString &href,
                                       const QString &name, bool isDir, qint64 sizeBytes,
                                       const QDateTime &modified, const QString &etag ) const
{
    QString s;
    s += QStringLiteral( "<%1response>\n" ).arg( prefix );
    s += QStringLiteral( "<%1href>%2</%1href>\n" ).arg( prefix, href.toHtmlEscaped() );
    s += QStringLiteral( "<%1propstat>\n<%1prop>\n" ).arg( prefix );
    if( !name.isEmpty() ) {
        s += QStringLiteral( "<%1displayname>%2</%1displayname>\n" )
                 .arg( prefix, name.toHtmlEscaped() );
    }
    if( !isDir && sizeBytes >= 0 ) {
        s += QStringLiteral( "<%1getcontentlength>%2</%1getcontentlength>\n" )
                 .arg( prefix )
                 .arg( sizeBytes );
    }
    if( modified.isValid() ) {
        // RFC 1123, the profile of RFC 2822's date-time grammar HTTP/WebDAV
        // use. English day/month names regardless of locale (Qt's
        // format-string toString(), unlike its QLocale-based overload).
        s += QStringLiteral( "<%1getlastmodified>%2</%1getlastmodified>\n" )
                 .arg( prefix,
                       modified.toUTC().toString(
                           QStringLiteral( "ddd, dd MMM yyyy HH:mm:ss 'GMT'" ) ) );
    }
    if( !etag.isEmpty() ) {
        s += QStringLiteral( "<%1getetag>%2</%1getetag>\n" ).arg( prefix, etag.toHtmlEscaped() );
    }
    s += QStringLiteral( "<%1resourcetype>%2</%1resourcetype>\n" )
             .arg( prefix,
                   isDir ? QStringLiteral( "<%1collection/>" ).arg( prefix ) : QString() );
    s += QStringLiteral( "</%1prop>\n<%1status>HTTP/1.1 200 OK</%1status>\n</%1propstat>\n" )
             .arg( prefix );
    s += QStringLiteral( "</%1response>\n" ).arg( prefix );
    return s;
}

QByteArray SWebDavStub::buildMultistatus( const QString &dirPath,
                                          const QVector<SWebDavStubEntry> &entries ) const
{
    const QString pfx = namespacePrefix_.isEmpty() ? QString()
                                                    : namespacePrefix_ + QLatin1Char( ':' );
    const QString xmlnsAttr = namespacePrefix_.isEmpty()
                                   ? QStringLiteral( " xmlns=\"DAV:\"" )
                                   : QStringLiteral( " xmlns:%1=\"DAV:\"" ).arg( namespacePrefix_ );

    QString out;
    out += QStringLiteral( "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n" );
    out += QStringLiteral( "<%1multistatus%2>\n" ).arg( pfx, xmlnsAttr );

    // The requested collection's OWN entry -- excluded from the parsed
    // result (gate 4 AC 2), but present here as any real WebDAV server sends
    // it on a Depth: 1 PROPFIND.
    out += responseFragment( pfx, hrefFor( dirPath, QString(), true ), QString(), true, -1,
                             QDateTime(), QString() );

    for( const SWebDavStubEntry &e : entries ) {
        out += responseFragment( pfx, hrefFor( dirPath, e.name, e.isDir ), e.name, e.isDir,
                                 e.sizeBytes, e.modified, e.etag );
    }

    out += QStringLiteral( "</%1multistatus>\n" ).arg( pfx );
    return out.toUtf8();
}
