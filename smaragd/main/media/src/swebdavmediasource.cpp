#include "app/media/swebdavmediasource.h"

#include "app/media/slocalmediasource.h"   // the shared bounds: kMaxSearchEntries,
                                            // kMaxSearchDepth, kMaxInFlightPropfinds,
                                            // kBatchEntries, kBatchMs (§B.2 inv. 4)
#include "app/media/smediatypes.h"

#include "tw/core/twlog.h"

#include <utility>

SWebDavMediaSource::SWebDavMediaSource( QString accountId, QUrl baseUrl,
                                        QString authorizationHeader, QObject *parent )
    : SMediaSource( parent )
    , accountId_( std::move( accountId ) )
    , client_( std::move( baseUrl ), std::move( authorizationHeader ) )
{
    connect( &client_, &SWebDavClient::propfindFinished, this,
             &SWebDavMediaSource::onPropfindFinished );
    connect( &client_, &SWebDavClient::propfindFailed, this,
             &SWebDavMediaSource::onPropfindFailed );
    connect( &client_, &SWebDavClient::getProgress, this,
             &SWebDavMediaSource::onGetProgress );
    connect( &client_, &SWebDavClient::getFinished, this,
             &SWebDavMediaSource::onGetFinished );
    connect( &client_, &SWebDavClient::getFailed, this,
             &SWebDavMediaSource::onGetFailed );
}

// client_ is a plain member (not a pointer): its own destructor aborts every
// outstanding reply (swebdavclient.cpp), which is what gate 4 AC 7 checks --
// destroying a source with requests in flight must not crash.
SWebDavMediaSource::~SWebDavMediaSource() = default;

QString SWebDavMediaSource::id() const
{
    return QStringLiteral( "nextcloud:%1" ).arg( accountId_ );
}

QString SWebDavMediaSource::displayName() const
{
    return tr( "Nextcloud (%1)" ).arg( accountId_ );
}

int SWebDavMediaSource::caps() const
{
    // NeedsFetch: a remote file must be downloaded before it can be placed.
    // No CanStream, for the same reason SLocalMediaSource has none -- the
    // MVP places files by (fetched, local) path, it does not read bytes
    // through a source.
    return CanBrowse | CanSearch | NeedsFetch;
}

int SWebDavMediaSource::liveRequestCount() const
{
    return live_.size();
}

QString SWebDavMediaSource::normalisedPath( const QString &path )
{
    QString p = path;
    while( p.startsWith( QLatin1Char( '/' ) ) ) p.remove( 0, 1 );
    while( p.endsWith( QLatin1Char( '/' ) ) ) p.chop( 1 );
    return p;
}

SMediaEntry SWebDavMediaSource::toMediaEntry( const SWebDavClient::Entry &e ) const
{
    SMediaEntry se;
    se.name      = e.name;
    se.ref       = SMediaRef( id(), e.href );
    se.isDir     = e.isDir;
    se.sizeBytes = e.sizeBytes;
    se.modified  = e.modified;
    se.etag      = e.etag;
    return se;
}

int SWebDavMediaSource::listDirectory( const QString &dirPath )
{
    const int logicalId = nextRequestId();

    SearchState st;
    st.logicalId  = logicalId;
    st.isListOnly = true;
    st.recursive  = false;
    st.suffixes   = suffixFilter();
    st.queue.enqueue( { normalisedPath( dirPath ), 0 } );
    st.sinceFlush.start();

    live_.insert( logicalId );
    searches_.insert( logicalId, std::move( st ) );
    pumpSearch( searches_[logicalId] );
    return logicalId;
}

int SWebDavMediaSource::search( const QString &rootPath, const QString &needle,
                                bool recursive, const QStringList &suffixes )
{
    const int logicalId = nextRequestId();

    SearchState st;
    st.logicalId  = logicalId;
    st.isListOnly = false;
    st.recursive  = recursive;
    st.needle     = needle;
    // The search request carries its OWN suffix list -- a snapshot of the
    // whole query, so a filter change mid-walk cannot make a result disagree
    // with the request that asked for it (same rule as the local provider).
    st.suffixes   = suffixes;
    st.queue.enqueue( { normalisedPath( rootPath ), 0 } );
    st.sinceFlush.start();

    live_.insert( logicalId );
    searches_.insert( logicalId, std::move( st ) );
    pumpSearch( searches_[logicalId] );
    return logicalId;
}

int SWebDavMediaSource::fetch( const QString &filePath, const QString &destPath )
{
    const int logicalId = nextRequestId();
    const int clientId  = client_.get( filePath, destPath );
    getOwner_.insert( clientId, logicalId );
    fetchClientByLogical_.insert( logicalId, clientId );
    live_.insert( logicalId );
    return logicalId;
}

void SWebDavMediaSource::cancel( int requestId )
{
    if( !live_.contains( requestId ) ) return;   // unknown id: a no-op, by contract
    live_.remove( requestId );

    auto sIt = searches_.find( requestId );
    if( sIt != searches_.end() ) {
        SearchState st = std::move( sIt.value() );
        searches_.erase( sIt );
        for( auto fi = st.inFlight.begin(); fi != st.inFlight.end(); ++fi ) {
            propfindOwner_.remove( fi.key() );
            client_.cancel( fi.key() );
        }
        return;
    }

    const int clientId = fetchClientByLogical_.take( requestId );
    if( clientId != 0 ) {
        getOwner_.remove( clientId );
        client_.cancel( clientId );
    }
}

void SWebDavMediaSource::pumpSearch( SearchState &st )
{
    while( st.inFlight.size() < smedia::kMaxInFlightPropfinds && !st.queue.isEmpty() ) {
        const SearchState::Pending p = st.queue.dequeue();
        const int                  clientId = client_.propfind( p.path );
        st.inFlight.insert( clientId, p.depth );
        propfindOwner_.insert( clientId, st.logicalId );
    }
    if( st.queue.isEmpty() && st.inFlight.isEmpty() ) finishSearch( st.logicalId );
}

void SWebDavMediaSource::appendToSearch( SearchState &st, const SMediaEntry &entry )
{
    if( st.delivered + st.pendingBatch.size() >= smedia::kMaxSearchEntries ) {
        // §B.2 inv. 4: truncated is a LOWER BOUND on what was not delivered
        // -- the walk stops here, so what lies beyond is by definition
        // uncounted.
        ++st.truncated;
        st.capHit = true;
        return;
    }
    st.pendingBatch.append( entry );
    if( st.pendingBatch.size() >= smedia::kBatchEntries
        || ( st.sinceFlush.isValid() && st.sinceFlush.elapsed() >= smedia::kBatchMs ) ) {
        flushSearch( st, false );
    }
}

void SWebDavMediaSource::flushSearch( SearchState &st, bool final )
{
    // Emits synchronously (both ends of this connection live on the main
    // thread, so Qt::AutoConnection resolves to Direct). A slot must not
    // call cancel() on THIS SAME request id from inside its entriesReady()
    // handler -- nothing in this module does; a caller that needs to should
    // route it through a queued call.
    if( !final && st.pendingBatch.isEmpty() ) return;
    st.delivered += st.pendingBatch.size();
    const QVector<SMediaEntry> batch = st.pendingBatch;
    st.pendingBatch.clear();
    st.sinceFlush.restart();
    emit entriesReady( st.logicalId, batch, final, final ? st.truncated : 0 );
}

void SWebDavMediaSource::finishSearch( int logicalId )
{
    auto it = searches_.find( logicalId );
    if( it == searches_.end() ) return;

    // Copy out and erase BEFORE the final emit -- mirrors the local
    // provider's onWorkerBatch (endRequest() before emit entriesReady()):
    // a slot that reacts to the final batch must see the request already
    // gone from liveRequestCount()/isLive(), never a moment later.
    SearchState st = std::move( it.value() );
    searches_.erase( it );
    live_.remove( logicalId );

    if( st.truncated > 0 ) {
        TW_LOGW( "media",
                 "webdav %s: request %d delivered %d entries and dropped at "
                 "least %d (caps: %d entries, depth %d, %d PROPFINDs in flight)",
                 accountId_.toUtf8().constData(), logicalId,
                 int( st.delivered + st.pendingBatch.size() ), st.truncated,
                 smedia::kMaxSearchEntries, smedia::kMaxSearchDepth,
                 smedia::kMaxInFlightPropfinds );
    }

    st.delivered += st.pendingBatch.size();
    emit entriesReady( logicalId, st.pendingBatch, true, st.truncated );
}

void SWebDavMediaSource::onPropfindFinished( int clientReqId,
                                             const QVector<SWebDavClient::Entry> &entries )
{
    const auto ownerIt = propfindOwner_.find( clientReqId );
    if( ownerIt == propfindOwner_.end() ) return;   // not one of ours (or superseded)
    const int logicalId = ownerIt.value();
    propfindOwner_.erase( ownerIt );

    const auto stIt = searches_.find( logicalId );
    if( stIt == searches_.end() ) return;   // cancelled already
    SearchState &st = stIt.value();

    st.anyCompleted = true;
    // The PARENT directory's depth, captured before its inFlight entry is
    // popped -- every child discovered in this response is one level deeper.
    const int parentDepth = st.inFlight.value( clientReqId, 0 );
    st.inFlight.remove( clientReqId );

    for( const SWebDavClient::Entry &e : entries ) {
        if( st.capHit ) break;

        if( e.isDir ) {
            if( st.isListOnly ) {
                // A browse listing shows every directory, unfiltered -- a
                // user must be able to navigate past a suffix filter into a
                // folder it hides (same rule the local provider follows).
                appendToSearch( st, toMediaEntry( e ) );
            } else if( st.recursive ) {
                const int childDepth = parentDepth + 1;
                if( childDepth > smedia::kMaxSearchDepth ) {
                    ++st.truncated;   // one bound per branch not taken
                } else {
                    st.queue.enqueue( { e.href, childDepth } );
                }
            }
            // Non-recursive: a directory is neither a result nor something
            // to descend into (AC 3: "non-recursive issues exactly one
            // PROPFIND").
            continue;
        }

        if( !smedia::suffixAccepted( e.name, st.suffixes ) ) continue;
        if( !st.isListOnly && !st.needle.isEmpty()
            && !e.name.contains( st.needle, Qt::CaseInsensitive ) ) {
            continue;
        }
        appendToSearch( st, toMediaEntry( e ) );
    }

    pumpSearch( st );
}

void SWebDavMediaSource::onPropfindFailed( int clientReqId, int httpStatus,
                                           const QString &message )
{
    const auto ownerIt = propfindOwner_.find( clientReqId );
    if( ownerIt == propfindOwner_.end() ) return;
    const int logicalId = ownerIt.value();
    propfindOwner_.erase( ownerIt );

    const auto stIt = searches_.find( logicalId );
    if( stIt == searches_.end() ) return;   // cancelled already
    SearchState &st = stIt.value();

    st.inFlight.remove( clientReqId );

    if( !st.anyCompleted ) {
        // The ROOT PROPFIND failing fails the whole request (§B.7).
        st.anyCompleted = true;
        searches_.erase( stIt );
        live_.remove( logicalId );
        TW_LOGW( "media", "webdav %s: request %d failed (HTTP %d): %s",
                 accountId_.toUtf8().constData(), logicalId, httpStatus,
                 message.toUtf8().constData() );
        emit requestFailed( logicalId, message );
        return;
    }

    // A SUBDIRECTORY PROPFIND failing mid-walk is one skipped branch, not a
    // failed search -- the same distinction the local walker makes for a
    // directory that vanished between being listed and being descended into.
    ++st.truncated;
    pumpSearch( st );
}

void SWebDavMediaSource::onGetProgress( int clientReqId, qint64 done, qint64 total )
{
    const auto it = getOwner_.find( clientReqId );
    if( it == getOwner_.end() || !live_.contains( it.value() ) ) return;
    emit fetchProgress( it.value(), done, total );
}

void SWebDavMediaSource::onGetFinished( int clientReqId, const QString &localPath )
{
    const auto it = getOwner_.find( clientReqId );
    if( it == getOwner_.end() ) return;
    const int logicalId = it.value();
    getOwner_.erase( it );
    fetchClientByLogical_.remove( logicalId );
    if( !live_.contains( logicalId ) ) return;   // cancelled/superseded
    live_.remove( logicalId );
    emit fetchFinished( logicalId, localPath );
}

void SWebDavMediaSource::onGetFailed( int clientReqId, int httpStatus, const QString &message )
{
    const auto it = getOwner_.find( clientReqId );
    if( it == getOwner_.end() ) return;
    const int logicalId = it.value();
    getOwner_.erase( it );
    fetchClientByLogical_.remove( logicalId );
    if( !live_.contains( logicalId ) ) return;
    live_.remove( logicalId );
    TW_LOGW( "media", "webdav %s: fetch %d failed (HTTP %d): %s",
             accountId_.toUtf8().constData(), logicalId, httpStatus,
             message.toUtf8().constData() );
    emit requestFailed( logicalId, message );
}
