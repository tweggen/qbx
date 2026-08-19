#include "app/shell/smediaaccountmanager.h"

#include "app/shell/ssecretstore.h"
#include "app/shell/ssettings.h"
#include "app/media/smediaregistry.h"
#include "app/media/swebdavclient.h"
#include "app/media/swebdavmediasource.h"

#include "tw/core/twlog.h"

#include <QByteArray>
#include <QEventLoop>
#include <QTimer>
#include <QUrl>

namespace {
const QString kSourcePrefix = QStringLiteral( "nextcloud:" );
}

QString SMediaAccountManager::secretKey( const QString &accountId )
{
    // NOT "<id>/password" -- SSecretStore appends "/passwordScheme" and
    // "/passwordEnc" to whatever key it is given (see its header), and §B.8's
    // on-disk shape is "media/nextcloud/<id>/passwordScheme", not a doubled
    // "/password/passwordScheme".
    return QStringLiteral( "media/nextcloud/" ) + accountId;
}

SMediaAccountManager::SMediaAccountManager( QObject *parent )
    : QObject( parent )
    , secretSettings_( QSettings::IniFormat, QSettings::UserScope, "Smaragd", "smaragd" )
{
    secretStore_.reset( new SSecretStore( &secretSettings_, QStringLiteral( "com.smaragd.media" ) ) );
    smedia::installCredentialProvider( this );

    rescanForPlaintextMigration();

    // Re-register a live source for every persisted account so the browser's
    // source combo offers it immediately after startup (AC 7), whether or
    // not its password was remembered -- an un-remembered one registers with
    // no Authorization header and simply reports 401 until the user
    // re-enters it in Options -> Media.
    for( const QString &id : accountIds() ) {
        const SMediaAccountInfo info = account( id );
        QString password;
        if( secretStore_->retrieve( secretKey( id ), &password ) != SSecretStore::Result::Ok )
            password.clear();
        registerSourceWithPassword( id, info.user, info.url, password );
    }
}

SMediaAccountManager::~SMediaAccountManager()
{
    if( smedia::credentialProvider() == this ) smedia::installCredentialProvider( nullptr );
    // sources_ entries are QObject children of `this`; Qt's parent-child
    // teardown deletes them, and SMediaRegistry drops each one via its own
    // destroyed() connection (main/media/CONTRACT.md: "the registry does not
    // own its sources").
}

QStringList SMediaAccountManager::accountIds() const
{
    QStringList ids = SSettings::instance().mediaNextcloudAccountIds();
    ids.sort();
    ids.removeDuplicates();
    return ids;
}

SMediaAccountInfo SMediaAccountManager::account( const QString &accountId ) const
{
    SMediaAccountInfo info;
    info.accountId = accountId;
    info.url       = SSettings::instance().mediaNextcloudUrl( accountId );
    info.user      = SSettings::instance().mediaNextcloudUser( accountId );

    QString unused;
    switch( secretStore_->retrieve( secretKey( accountId ), &unused ) ) {
    case SSecretStore::Result::Ok:            info.passwordStatus = SMediaPasswordStatus::Set; break;
    case SSecretStore::Result::Unset:         info.passwordStatus = SMediaPasswordStatus::Unset; break;
    case SSecretStore::Result::Undecryptable: info.passwordStatus = SMediaPasswordStatus::Undecryptable; break;
    }
    return info;
}

QString SMediaAccountManager::basicHeader( const QString &user, const QString &password ) const
{
    if( user.isEmpty() && password.isEmpty() ) return QString();
    const QByteArray raw = ( user + QLatin1Char( ':' ) + password ).toUtf8();
    return QStringLiteral( "Basic " ) + QString::fromLatin1( raw.toBase64() );
}

bool SMediaAccountManager::setAccount( const QString &accountId, const QString &url,
                                       const QString &user, const QString &password,
                                       bool remember, QString *error )
{
    if( accountId.trimmed().isEmpty() ) {
        if( error ) *error = QStringLiteral( "an account needs an id" );
        return false;
    }

    // AC 13: refused HERE, before a single byte is persisted or a single
    // request is issued -- never discovered later as "the first request
    // failed".
    const QUrl parsed( url, QUrl::StrictMode );
    const bool validScheme = parsed.scheme() == QLatin1String( "http" )
                            || parsed.scheme() == QLatin1String( "https" );
    if( url.trimmed().isEmpty() || !parsed.isValid() || parsed.host().isEmpty() || !validScheme ) {
        if( error ) *error = QStringLiteral( "not a valid http(s) URL" );
        TW_LOGW( "media", "account '%s': refused an unparseable URL '%s' (AC 13)",
                 accountId.toUtf8().constData(), url.toUtf8().constData() );
        return false;
    }

    SSettings &s = SSettings::instance();
    QStringList ids = s.mediaNextcloudAccountIds();
    if( !ids.contains( accountId ) ) {
        ids << accountId;
        s.setMediaNextcloudAccountIds( ids );
    }
    s.setMediaNextcloudUrl( accountId, url );
    s.setMediaNextcloudUser( accountId, user );

    const QString key = secretKey( accountId );
    if( remember && secretStore_->canPersist() ) {
        if( !secretStore_->store( key, password ) )
            TW_LOGW( "media", "account '%s': the secret store refused to remember the password; "
                              "it is still usable for this session only",
                     accountId.toUtf8().constData() );
    } else {
        // "Remember off means off" (§B.8 rule 4): no key of any kind is
        // written, and switching Remember from on to off on an existing
        // account SCRUBS whatever was there -- a checkbox that only ever
        // adds a secret and never removes one would not really mean "off".
        secretStore_->remove( key );
    }

    registerSourceWithPassword( accountId, user, url, password );
    return true;
}

void SMediaAccountManager::removeAccount( const QString &accountId )
{
    if( accountId.isEmpty() ) return;
    unregisterSource( accountId );
    secretStore_->remove( secretKey( accountId ) );   // AC 11: the secret itself, too

    // BELT AND SUSPENDERS, deliberately outside SSecretStore: its remove()
    // is a no-op for the `none`/`memory` backends -- correct on its own
    // terms (there is nothing THIS backend ever wrote), but it means an
    // account remembered by an EARLIER process under a REAL backend (dpapi/
    // keychain/libsecret) and then removed by a LATER process running under
    // `none` (SMARAGD_SECRET_BACKEND changed, or the platform store became
    // unavailable) would leave its passwordScheme/passwordEnc pointer
    // sitting in the INI forever -- AC 11 says "every key", not "every key
    // this backend recognises". Scrubbing them here, unconditionally, is
    // always safe: for the common case (remove under the SAME backend that
    // wrote it) SSecretStore::remove() already did this and these are
    // idempotent no-ops. What this canNOT do is purge a keychain/libsecret
    // ITEM left by an earlier real-backend run when the CURRENT process has
    // no platform API to reach it (`none`) -- that orphans the platform
    // entry rather than deleting it, a narrower and platform-specific gap
    // than AC 11 promises, called out rather than silently accepted.
    SSettings::instance().remove( secretKey( accountId ) + QStringLiteral( "/passwordScheme" ) );
    SSettings::instance().remove( secretKey( accountId ) + QStringLiteral( "/passwordEnc" ) );

    SSettings &s = SSettings::instance();
    QStringList ids = s.mediaNextcloudAccountIds();
    if( ids.removeAll( accountId ) > 0 ) {
        // Removed rather than set to an empty list: a leftover
        // "media/nextcloud/accounts=" key where none existed before is new
        // content the INI-restoration discipline (a RUN_SERIAL qxa case must
        // leave smaragd.ini exactly as it found it) does not allow, even
        // though it names no account.
        if( ids.isEmpty() ) s.remove( QStringLiteral( "media/nextcloud/accounts" ) );
        else s.setMediaNextcloudAccountIds( ids );
    }
    s.removeMediaNextcloudAccountFields( accountId );
}

QString SMediaAccountManager::secretBackendName() const
{
    return secretStore_->backendName();
}

bool SMediaAccountManager::canRememberPasswords() const
{
    return secretStore_->canPersist();
}

QString SMediaAccountManager::backendDescription() const
{
    const QString b = secretBackendName();
    if( b == QLatin1String( "dpapi" ) )
        return QStringLiteral( "Passwords are stored with Windows DPAPI, protected by your login." );
    if( b == QLatin1String( "keychain" ) )
        return QStringLiteral( "Passwords are stored in the macOS login keychain." );
    if( b == QLatin1String( "libsecret" ) )
        return QStringLiteral( "Passwords are stored in the Secret Service (libsecret)." );
    if( b == QLatin1String( "memory" ) )
        return QStringLiteral( "Test backend: passwords are kept in memory for this process only." );
    return QStringLiteral(
        "This system has no credential store -- the password is kept for this session only." );
}

QString SMediaAccountManager::testConnection( const QString &url, const QString &user,
                                              const QString &password, int timeoutMs )
{
    return testWithHeader( url, basicHeader( user, password ), timeoutMs );
}

QString SMediaAccountManager::testAccountConnection( const QString &accountId, const QString &url,
                                                     const QString &user, const QString &password,
                                                     int timeoutMs )
{
    // A typed password always wins: it is the one thing the user can see, and
    // testing anything else while a password is on screen would be a lie.
    if( !password.isEmpty() ) return testConnection( url, user, password, timeoutMs );

    const QString id = accountId.trimmed();
    if( id.isEmpty() || !accountIds().contains( id ) )
        return QStringLiteral( "enter the app password to test" );

    const SMediaAccountInfo info = account( id );
    if( info.url != url.trimmed() || info.user != user )
        return QStringLiteral( "the URL or username has changed - Save it, or type "
                               "the app password to test" );

    switch( info.passwordStatus ) {
    case SMediaPasswordStatus::Unset:
        // Includes Remember-off accounts: the session password lives in the
        // registered source's header and deliberately nowhere this method can
        // reach it, because the only accessor that could hand it back would
        // be an accessor that hands a credential back (AC 14).
        return QStringLiteral( "no saved password for this account - type it to test" );
    case SMediaPasswordStatus::Undecryptable:
        return QStringLiteral( "the saved password cannot be read on this machine - "
                               "re-enter it" );
    case SMediaPasswordStatus::Set:
        break;
    }

    // Exactly the header the browser itself would send for this source, so a
    // green Test and a working browse can never disagree.
    const QString header = authorizationHeaderFor( kSourcePrefix + id );
    if( header.isEmpty() )
        return QStringLiteral( "no usable saved password - re-enter it" );

    return testWithHeader( url, header, timeoutMs ) + QStringLiteral( " (saved password)" );
}

QString SMediaAccountManager::testWithHeader( const QString &url, const QString &authHeader,
                                              int timeoutMs )
{
    const QUrl parsed( url, QUrl::StrictMode );
    const bool validScheme = parsed.scheme() == QLatin1String( "http" )
                            || parsed.scheme() == QLatin1String( "https" );
    if( url.trimmed().isEmpty() || !parsed.isValid() || parsed.host().isEmpty() || !validScheme )
        return QStringLiteral( "invalid URL" );

    SWebDavClient client( parsed, authHeader );
    QString  result;
    bool     done = false;
    QEventLoop loop;
    QTimer   timeoutTimer;
    timeoutTimer.setSingleShot( true );

    connect( &timeoutTimer, &QTimer::timeout, &loop, [&]() {
        result = QStringLiteral( "timed out" );
        done   = true;
        loop.quit();
    } );
    connect( &client, &SWebDavClient::propfindFinished, &loop,
             [&]( int, const QVector<SWebDavClient::Entry> &entries ) {
        result = QStringLiteral( "200 OK (%1 entries)" ).arg( entries.size() );
        done   = true;
        loop.quit();
    } );
    connect( &client, &SWebDavClient::propfindFailed, &loop,
             [&]( int, int httpStatus, const QString &message ) {
        result = httpStatus > 0 ? QStringLiteral( "%1 %2" ).arg( httpStatus ).arg( message )
                                : QStringLiteral( "connection failed: %1" ).arg( message );
        done   = true;
        loop.quit();
    } );

    timeoutTimer.start( timeoutMs );
    client.propfind( QString() );
    loop.exec();

    return done ? result : QStringLiteral( "unknown failure" );
}

QString SMediaAccountManager::authorizationHeaderFor( const QString &sourceId ) const
{
    if( !sourceId.startsWith( kSourcePrefix ) ) return QString();
    const QString accountId = sourceId.mid( kSourcePrefix.size() );

    QString password;
    if( secretStore_->retrieve( secretKey( accountId ), &password ) != SSecretStore::Result::Ok )
        return QString();
    return basicHeader( SSettings::instance().mediaNextcloudUser( accountId ), password );
}

void SMediaAccountManager::registerSourceWithPassword( const QString &accountId, const QString &user,
                                                        const QString &url, const QString &password )
{
    unregisterSource( accountId );
    if( url.isEmpty() ) return;

    const QString header = password.isEmpty() ? QString() : basicHeader( user, password );
    auto *src = new SWebDavMediaSource( accountId, QUrl( url ), header, this );
    sources_.insert( accountId, src );
    SMediaRegistry::instance().registerSource( src );
}

void SMediaAccountManager::unregisterSource( const QString &accountId )
{
    auto it = sources_.find( accountId );
    if( it == sources_.end() ) return;
    SWebDavMediaSource *src = it.value();
    sources_.erase( it );
    SMediaRegistry::instance().unregisterSource( kSourcePrefix + accountId );
    delete src;
}

void SMediaAccountManager::rescanForPlaintextMigration()
{
    SSettings &s = SSettings::instance();
    for( const QString &id : s.mediaNextcloudAccountIds() ) {
        const QString plainKey = QStringLiteral( "media/nextcloud/" ) + id + QStringLiteral( "/password" );
        if( !s.contains( plainKey ) ) continue;

        const QString plain = s.value( plainKey ).toString();
        TW_LOGW( "media",
                 "account '%s': found a legacy PLAINTEXT password key on load "
                 "(only a pre-release dev build could have written one) -- migrating "
                 "it into the secret store and removing the plaintext key",
                 id.toUtf8().constData() );
        if( !plain.isEmpty() && !secretStore_->store( secretKey( id ), plain ) )
            TW_LOGW( "media",
                     "account '%s': the secret store could not accept the migrated "
                     "password; it will need to be re-entered",
                     id.toUtf8().constData() );
        // Removed either way (AC 17: "never left behind beside the
        // ciphertext") -- a store failure must not leave the plaintext as a
        // fallback.
        s.remove( plainKey );
    }
}
