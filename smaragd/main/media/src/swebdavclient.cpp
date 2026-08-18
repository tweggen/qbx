#include "app/media/swebdavclient.h"

#include "tw/core/twlog.h"

#include <QDomDocument>
#include <QDomElement>
#include <QFutureWatcher>
#include <QIODevice>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QSslError>
#include <QStringList>
#include <QtConcurrentRun>

#include <utility>

namespace {

// ---- namespace-agnostic DOM helpers -----------------------------------
//
// Real WebDAV servers spell the DAV namespace prefix differently ("d:",
// "D:", sometimes none at all with a default xmlns). Comparing by LOCAL
// NAME -- the tag with any "prefix:" stripped -- is what makes the parser
// agree with all of them without caring which one a given server picked.

QString localName( const QDomElement &el )
{
    const QString tag = el.tagName();
    const int     idx = tag.indexOf( QLatin1Char( ':' ) );
    return idx < 0 ? tag : tag.mid( idx + 1 );
}

QDomElement firstChildByLocalName( const QDomElement &parent, const QString &name )
{
    for( QDomElement c = parent.firstChildElement(); !c.isNull();
         c = c.nextSiblingElement() ) {
        if( localName( c ).compare( name, Qt::CaseInsensitive ) == 0 ) return c;
    }
    return QDomElement();
}

QVector<QDomElement> childrenByLocalName( const QDomElement &parent, const QString &name )
{
    QVector<QDomElement> out;
    for( QDomElement c = parent.firstChildElement(); !c.isNull();
         c = c.nextSiblingElement() ) {
        if( localName( c ).compare( name, Qt::CaseInsensitive ) == 0 ) out.append( c );
    }
    return out;
}

// ---- the off-thread parse (§B.7: a directory of 10 000 entries is a
// megabyte of XML) --------------------------------------------------------
//
// A plain free function taking everything it needs BY VALUE: it runs on a
// QtConcurrent worker thread and must not touch the client that queued it.

QVector<SWebDavClient::Entry> parsePropfindXml( QByteArray xml, QString basePathDecoded,
                                                QString selfRelPathNormalized )
{
    QVector<SWebDavClient::Entry> out;

    QDomDocument doc;
    QString      errMsg;
    int          errLine = 0, errCol = 0;
    if( !doc.setContent( xml, false, &errMsg, &errLine, &errCol ) ) {
        TW_LOGW( "media", "webdav: PROPFIND XML parse failed at %d:%d: %s", errLine,
                 errCol, errMsg.toUtf8().constData() );
        return out;
    }

    const QDomElement root = doc.documentElement();   // multistatus
    for( const QDomElement &resp : childrenByLocalName( root, QStringLiteral( "response" ) ) ) {
        const QDomElement hrefEl = firstChildByLocalName( resp, QStringLiteral( "href" ) );
        if( hrefEl.isNull() ) continue;

        // Percent-encoded hrefs are decoded HERE, once, at the one place a
        // path enters the app (gate 4 AC 2: "Drum%20Kit/kick%231.wav" ->
        // "Drum Kit/kick#1.wav").
        const QString hrefDecoded =
            QUrl::fromPercentEncoding( hrefEl.text().trimmed().toUtf8() );

        QString rel = hrefDecoded;
        if( rel.startsWith( basePathDecoded ) ) rel = rel.mid( basePathDecoded.length() );

        const bool dirByTrailingSlash = rel.endsWith( QLatin1Char( '/' ) );
        // Trailing slashes are normalised away (gate 4 AC 2): every ref.path
        // this client hands out is slash-free, directory or not.
        if( rel.endsWith( QLatin1Char( '/' ) ) ) rel.chop( 1 );

        // The request's own collection is excluded from its own listing
        // (gate 4 AC 2) -- a Depth: 1 PROPFIND's multistatus includes the
        // resource that was asked about as its first <response>.
        if( rel == selfRelPathNormalized ) continue;

        // Take the first propstat that reports 200 (or carries no <status>
        // at all -- some servers omit it on a single-propstat response); a
        // 404 propstat for an unsupported property is not the record we want.
        QDomElement prop;
        for( const QDomElement &ps :
             childrenByLocalName( resp, QStringLiteral( "propstat" ) ) ) {
            const QDomElement st = firstChildByLocalName( ps, QStringLiteral( "status" ) );
            if( st.isNull() || st.text().contains( QStringLiteral( "200" ) ) ) {
                prop = firstChildByLocalName( ps, QStringLiteral( "prop" ) );
                break;
            }
        }
        if( prop.isNull() ) continue;

        SWebDavClient::Entry e;
        e.href = rel;

        const QDomElement rtEl =
            firstChildByLocalName( prop, QStringLiteral( "resourcetype" ) );
        e.isDir = dirByTrailingSlash;
        if( !rtEl.isNull()
            && !firstChildByLocalName( rtEl, QStringLiteral( "collection" ) ).isNull() ) {
            e.isDir = true;
        }

        const QDomElement dnEl =
            firstChildByLocalName( prop, QStringLiteral( "displayname" ) );
        e.name = !dnEl.isNull() && !dnEl.text().trimmed().isEmpty()
                     ? dnEl.text().trimmed()
                     : rel.section( QLatin1Char( '/' ), -1 );

        // A missing optional property yields -1 / invalid rather than
        // dropping the entry (gate 4 AC 2) -- Entry's own defaults already
        // are -1 / invalid, so this is "only set it when it is there".
        const QDomElement clEl =
            firstChildByLocalName( prop, QStringLiteral( "getcontentlength" ) );
        if( !clEl.isNull() && !e.isDir ) {
            bool          ok  = false;
            const qint64  len = clEl.text().trimmed().toLongLong( &ok );
            if( ok ) e.sizeBytes = len;
        }

        const QDomElement lmEl =
            firstChildByLocalName( prop, QStringLiteral( "getlastmodified" ) );
        if( !lmEl.isNull() ) {
            // RFC 1123 ("Mon, 12 Jan 2026 10:00:00 GMT") is the profile of
            // RFC 2822's date-time grammar that HTTP/WebDAV servers use --
            // except RFC 1123 (unlike RFC 2822) always spells the zone as the
            // literal "GMT", which Qt::RFC2822Date's parser does not accept
            // (it wants a numeric offset or nothing). Try the strict parse
            // first, then substitute the equivalent "+0000" and retry --
            // never guess at a value Qt itself rejected outright.
            const QString raw = lmEl.text().trimmed();
            QDateTime     dt  = QDateTime::fromString( raw, Qt::RFC2822Date );
            if( !dt.isValid() && raw.endsWith( QStringLiteral( "GMT" ) ) ) {
                const QString withOffset =
                    raw.left( raw.length() - 3 ) + QStringLiteral( "+0000" );
                dt = QDateTime::fromString( withOffset, Qt::RFC2822Date );
            }
            e.modified = dt;
        }

        const QDomElement etEl = firstChildByLocalName( prop, QStringLiteral( "getetag" ) );
        if( !etEl.isNull() ) e.etag = etEl.text().trimmed();

        out.append( e );
    }
    return out;
}

} // namespace

SWebDavClient::SWebDavClient( QUrl baseUrl, QString authorizationHeader, QObject *parent )
    : QObject( parent )
    , nam_( new QNetworkAccessManager( this ) )
    , baseUrl_( std::move( baseUrl ) )
    , authHeader_( std::move( authorizationHeader ) )
{
    QString p = baseUrl_.path();
    if( !p.endsWith( QLatin1Char( '/' ) ) ) p += QLatin1Char( '/' );
    basePathDecoded_ = QUrl::fromPercentEncoding( p.toUtf8() );
}

SWebDavClient::~SWebDavClient()
{
    // Abort every outstanding reply (gate 4 AC 7, trap T3): a QNetworkReply
    // outliving this client is a use-after-free with a network-shaped delay
    // on it. The reply is parented to `this` and would be torn down by
    // ordinary QObject teardown regardless, but abort() stops the transfer
    // instead of merely detaching from it, and a Get's QSaveFile has to be
    // told to CANCEL explicitly -- letting it merely go out of scope without
    // cancelWriting() is exactly the "leftover .tmp" trap (gate 4 AC 5).
    //
    // requests_ is cleared BEFORE any abort(): some backends emit
    // finished()/error() synchronously from inside abort(), which would
    // re-enter finishPropfind()/finishGet() through the very state this loop
    // is iterating -- an iterator-invalidating double free. Emptying the map
    // first means a reentrant handler finds nothing and returns immediately.
    const QList<RequestState> pending = requests_.values();
    requests_.clear();
    for( const RequestState &st : pending ) {
        if( st.reply ) st.reply->abort();
        if( st.file ) {
            st.file->cancelWriting();
            delete st.file;
        }
    }
    // Replies are parented to `this`; ordinary QObject teardown deletes them
    // right after this destructor body finishes.
}

QUrl SWebDavClient::urlFor( const QString &relPath ) const
{
    // `relPath` is DECODED (SMediaRef's own rule: "never percent-encoded").
    // QUrl::setPath() takes a DECODED path and does its OWN percent-encoding
    // when it serialises the request line -- pre-encoding the segments here
    // (an earlier version of this function did, with QUrl::toPercentEncoding)
    // DOUBLE-encodes them: setPath() would then also escape the literal '%'
    // characters it was just handed, turning "Drum%20Kit" into
    // "Drum%2520Kit" on the wire. A space in a directory name silently 404'd
    // because of exactly this.
    QUrl    u    = baseUrl_;
    QString path = u.path();
    if( !path.endsWith( QLatin1Char( '/' ) ) ) path += QLatin1Char( '/' );
    path += relPath;
    u.setPath( path );
    return u;
}

int SWebDavClient::propfind( const QString &dirPath )
{
    const int id = ++nextId_;

    QNetworkRequest req( urlFor( dirPath ) );
    req.setRawHeader( "Depth", "1" );
    req.setRawHeader( "Content-Type", "application/xml; charset=utf-8" );
    if( !authHeader_.isEmpty() ) req.setRawHeader( "Authorization", authHeader_.toUtf8() );

    static const char *kBody =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<d:propfind xmlns:d=\"DAV:\">"
        "<d:prop>"
        "<d:displayname/><d:getcontentlength/><d:getlastmodified/>"
        "<d:getcontenttype/><d:resourcetype/><d:getetag/>"
        "</d:prop></d:propfind>";

    QNetworkReply *reply = nam_->sendCustomRequest( req, "PROPFIND", QByteArray( kBody ) );
    reply->setParent( this );   // trap T3: never let a reply outlive its source

    RequestState st;
    st.kind  = Kind::Propfind;
    st.reply = reply;
    requests_.insert( id, st );

    connect( reply, &QNetworkReply::finished, this, [this, id] { finishPropfind( id ); } );
    // §B.7: TLS errors are surfaced, never ignored. Deliberately no call
    // anywhere in this file tells Qt to accept the certificate error (gate 4
    // AC 8 greps the tree for that exact method name) -- leaving the errors
    // unhandled makes Qt fail the request, and finishPropfind()/finishGet()
    // report that through the *Failed signal.
    connect( reply, &QNetworkReply::sslErrors, this,
             []( const QList<QSslError> & ) {
                 // no-op by design: see the comment above.
             } );

    return id;
}

void SWebDavClient::finishPropfind( int requestId )
{
    auto it = requests_.find( requestId );
    if( it == requests_.end() ) return;   // cancelled and already forgotten

    QNetworkReply *reply = it->reply;

    if( reply->error() != QNetworkReply::NoError ) {
        const int     status = reply->attribute( QNetworkRequest::HttpStatusCodeAttribute ).toInt();
        const QString msg    = reply->errorString();
        requests_.erase( it );
        reply->deleteLater();
        // §B.7: a 401 does not retry. This is the ONLY place a PROPFIND
        // failure is reported, and nothing here loops back into propfind().
        emit propfindFailed( requestId, status, msg );
        return;
    }

    const QByteArray xml = reply->readAll();

    // The href of the request itself -- recovered from the reply's own
    // request URL rather than threaded through as a separate argument, so a
    // cancel()/reuse cannot desync it from the actual request that was sent.
    QString self = QUrl::fromPercentEncoding( reply->url().path().toUtf8() );
    if( self.startsWith( basePathDecoded_ ) ) self = self.mid( basePathDecoded_.length() );
    if( self.endsWith( QLatin1Char( '/' ) ) ) self.chop( 1 );

    requests_.erase( it );
    reply->deleteLater();

    // §B.7: parsed OFF the GUI thread. The watcher is parented to `this`, so
    // it is torn down (never leaked) if the client is destroyed before the
    // parse completes; the free function below touches nothing but its
    // by-value arguments, so the worker thread never reaches `this`.
    auto *watcher = new QFutureWatcher<QVector<SWebDavClient::Entry>>( this );
    connect( watcher, &QFutureWatcher<QVector<SWebDavClient::Entry>>::finished, this,
             [this, requestId, watcher] {
                 const QVector<SWebDavClient::Entry> entries = watcher->result();
                 watcher->deleteLater();
                 emit propfindFinished( requestId, entries );
             } );
    watcher->setFuture( QtConcurrent::run( parsePropfindXml, xml, basePathDecoded_, self ) );
}

int SWebDavClient::get( const QString &filePath, const QString &destPath )
{
    const int id = ++nextId_;

    auto *file = new QSaveFile( destPath );
    if( !file->open( QIODevice::WriteOnly ) ) {
        const QString msg = QStringLiteral( "cannot open '%1' for writing: %2" )
                                .arg( destPath, file->errorString() );
        delete file;
        QMetaObject::invokeMethod(
            this, [this, id, msg] { emit getFailed( id, 0, msg ); }, Qt::QueuedConnection );
        return id;
    }

    QNetworkRequest req( urlFor( filePath ) );
    if( !authHeader_.isEmpty() ) req.setRawHeader( "Authorization", authHeader_.toUtf8() );

    QNetworkReply *reply = nam_->get( req );
    reply->setParent( this );   // trap T3

    RequestState st;
    st.kind     = Kind::Get;
    st.reply    = reply;
    st.file     = file;
    st.destPath = destPath;
    requests_.insert( id, st );

    connect( reply, &QNetworkReply::readyRead, this, [this, id, reply] {
        auto it = requests_.find( id );
        if( it != requests_.end() && it->file ) it->file->write( reply->readAll() );
    } );
    connect( reply, &QNetworkReply::downloadProgress, this,
             [this, id]( qint64 done, qint64 total ) {
                 if( requests_.contains( id ) ) emit getProgress( id, done, total );
             } );
    connect( reply, &QNetworkReply::finished, this, [this, id] { finishGet( id ); } );
    connect( reply, &QNetworkReply::sslErrors, this,
             []( const QList<QSslError> & ) { /* §B.7: never ignored */ } );

    return id;
}

void SWebDavClient::finishGet( int requestId )
{
    auto it = requests_.find( requestId );
    if( it == requests_.end() ) return;   // cancelled and already forgotten

    QNetworkReply *reply = it->reply;
    QSaveFile     *file  = it->file;

    // Qt does not guarantee a final readyRead before finished() -- drain
    // whatever is still buffered first.
    if( file && file->isOpen() ) file->write( reply->readAll() );

    if( reply->error() != QNetworkReply::NoError ) {
        // A cancelled or interrupted GET leaves no partial file and no
        // .tmp behind (gate 4 ACs 4, 5): cancelWriting(), never commit().
        if( file ) {
            file->cancelWriting();
            delete file;
        }
        const int     status = reply->attribute( QNetworkRequest::HttpStatusCodeAttribute ).toInt();
        const QString msg    = reply->errorString();
        requests_.erase( it );
        reply->deleteLater();
        emit getFailed( requestId, status, msg );
        return;
    }

    QString destPath;
    if( file ) {
        destPath = file->fileName();
        if( !file->commit() ) {
            const QString msg =
                QStringLiteral( "could not finalise '%1': %2" ).arg( destPath, file->errorString() );
            delete file;
            requests_.erase( it );
            reply->deleteLater();
            emit getFailed( requestId, 0, msg );
            return;
        }
        delete file;
    }

    requests_.erase( it );
    reply->deleteLater();
    emit getFinished( requestId, destPath );
}

void SWebDavClient::cancel( int requestId )
{
    auto it = requests_.find( requestId );
    if( it == requests_.end() ) return;   // unknown id: a no-op, by contract

    // Erase BEFORE abort(): see the destructor's comment -- a backend that
    // emits finished() synchronously from inside abort() would otherwise
    // re-enter finishPropfind()/finishGet() through this very entry.
    const RequestState st = *it;
    requests_.erase( it );

    if( st.reply ) {
        st.reply->abort();
        st.reply->deleteLater();
    }
    if( st.file ) {
        st.file->cancelWriting();
        delete st.file;
    }
}
