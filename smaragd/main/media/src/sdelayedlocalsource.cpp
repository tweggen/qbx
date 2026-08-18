#include "app/media/sdelayedlocalsource.h"

#include "tw/core/twlog.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QTimer>

bool SDelayedLocalSource::isEnabled()
{
    static const bool on =
        qgetenv( "SMARAGD_MEDIA_TEST_SOURCE" ) == QByteArray( "1" );
    return on;
}

QString SDelayedLocalSource::sourceIdString()
{
    return QStringLiteral( "testdelay" );
}

SDelayedLocalSource::SDelayedLocalSource( QObject *parent )
    : SLocalMediaSource( parent )
{
    TW_LOGI( "media", "test source 'testdelay' registered "
                      "(SMARAGD_MEDIA_TEST_SOURCE=1)" );
}

SDelayedLocalSource::~SDelayedLocalSource() = default;

QString SDelayedLocalSource::id() const { return sourceIdString(); }

QString SDelayedLocalSource::displayName() const
{
    return tr( "Delayed local (test)" );
}

int SDelayedLocalSource::caps() const
{
    return CanBrowse | CanSearch | NeedsFetch;
}

void SDelayedLocalSource::setFetchDelayMs( int ms )
{
    delayMs_ = ms < 0 ? 0 : ms;
}

void SDelayedLocalSource::setFailSubstring( const QString &needle )
{
    failNeedle_ = needle;
}

int SDelayedLocalSource::fetch( const QString &filePath,
                                const QString &destPath )
{
    const int     id   = nextRequestId();
    const QString src  = QDir::fromNativeSeparators( filePath );
    const QString dest = QDir::fromNativeSeparators( destPath );
    liveFetches_.insert( id, true );

    // A QTimer, not a sleep: everything in this module is async by construction
    // and a blocking fetch would make the deferred branch synchronous — the one
    // thing the class exists to avoid. `this` is the context object, so a
    // source destroyed before the timer fires simply never sees it.
    QPointer<SDelayedLocalSource> self( this );
    QTimer::singleShot( delayMs_, this, [ self, id, src, dest ] {
        if( !self ) return;
        if( !self->liveFetches_.remove( id ) ) return;   // cancelled

        if( !self->failNeedle_.isEmpty() && src.contains( self->failNeedle_ ) ) {
            emit self->requestFailed(
                id, QStringLiteral( "the test source was told to fail on '%1'" )
                        .arg( self->failNeedle_ ) );
            return;
        }
        if( !QFileInfo::exists( src ) ) {
            emit self->requestFailed(
                id, QStringLiteral( "no such file: %1" ).arg( src ) );
            return;
        }
        QDir().mkpath( QFileInfo( dest ).absolutePath() );
        QFile::remove( dest );
        if( !QFile::copy( src, dest ) ) {
            emit self->requestFailed(
                id, QStringLiteral( "could not copy %1 to %2" ).arg( src, dest ) );
            return;
        }
        ++self->fetchCount_;
        emit self->fetchProgress( id, QFileInfo( dest ).size(),
                                  QFileInfo( dest ).size() );
        emit self->fetchFinished( id, dest );
    } );
    return id;
}

void SDelayedLocalSource::cancel( int requestId )
{
    // A fetch id and a walk id come out of the SAME monotone counter, so at
    // most one of these two can match — the other is a no-op by contract.
    liveFetches_.remove( requestId );
    SLocalMediaSource::cancel( requestId );
}
