// SSecretStore dispatch, plus the two backends that need no platform code:
// `memory` (a real, process-lifetime store, used by real backend-round-trip
// tests and as the `--test-case` default) and `none` (the honest "nothing is
// persisted" fallback -- see app/shell/ssecretstore.h and proposal 38 §B.8).
//
// The `dpapi` / `keychain` / `libsecret` backends live in their own
// per-platform translation units (ssecretstore_win.cpp / _mac.mm /
// _linux.cpp) behind ssecretstorebackend.h's seam, so this file compiles
// identically on every platform and only its #if branches differ in what
// they can reach.

#include "app/shell/ssecretstore.h"
#include "ssecretstorebackend.h"

#include <QByteArray>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QSettings>

#include <cstdlib>
#include <utility>

#include "tw/core/twlog.h"

namespace {

QString lowered( const QString &s )
{
    return s.trimmed().toLower();
}

// The platform's compiled-in real backend, in preference order. Only ever
// returns the name of a backend this translation unit can ACTUALLY serve --
// never a name whose #if branch is missing, which is what would turn "ask
// for the default" into a silent Undecryptable loop.
QString platformDefaultBackend()
{
#if defined( TW_HAVE_DPAPI )
    return QStringLiteral( "dpapi" );
#elif defined( TW_HAVE_KEYCHAIN )
    return QStringLiteral( "keychain" );
#elif defined( TW_HAVE_LIBSECRET )
    return QStringLiteral( "libsecret" );
#else
    return QStringLiteral( "none" );
#endif
}

void warnUnavailable( const QString &want )
{
    TW_LOGW( "secretstore",
             "secret backend '%s' is not available in this build; using the "
             "platform default",
             want.toUtf8().constData() );
}

void warnUnknown( const QString &want )
{
    TW_LOGW( "secretstore",
             "unknown SMARAGD_SECRET_BACKEND='%s' (expected "
             "dpapi|keychain|libsecret|none|memory|default); using the "
             "platform default",
             want.toUtf8().constData() );
}

// The `memory` backend (§B.8 rule 5): "a real store with process lifetime".
// A static map rather than a per-instance one is deliberate -- the
// options-page cases (5b) open a fresh dialog, hence a fresh SSecretStore
// instance, per gesture, and still expect a "remembered" account to still be
// there without a second process. Keyed by service+key so two instances
// scoped to different service names (AC 6a) cannot see each other's secrets
// even within the same process.
QMutex g_memoryMutex;
QHash<QString, QString> g_memoryStore;

QString memoryMapKey( const QString &service, const QString &key )
{
    return service + QLatin1Char( '\x1f' ) + key;
}

} // namespace

SSecretStore::SSecretStore( QSettings *settings, QString serviceName, const QString &backendOverride )
    : settings_( settings )
    , service_( std::move( serviceName ) )
    , backend_( resolveBackend( backendOverride ) )
{
}

SSecretStore::~SSecretStore() = default;

QString SSecretStore::resolveBackend( const QString &backendOverride )
{
    QString want = backendOverride;
    if( want.isEmpty() ) {
        if( const char *env = std::getenv( "SMARAGD_SECRET_BACKEND" ) )
            want = QString::fromLatin1( env );
    }
    want = lowered( want );
    if( want.isEmpty() ) want = QStringLiteral( "default" );

    if( want == QLatin1String( "memory" ) ) return want;
    if( want == QLatin1String( "none" ) )   return want;

    if( want == QLatin1String( "dpapi" ) ) {
#if defined( TW_HAVE_DPAPI )
        return want;
#else
        warnUnavailable( want );
        return platformDefaultBackend();
#endif
    }
    if( want == QLatin1String( "keychain" ) ) {
#if defined( TW_HAVE_KEYCHAIN )
        return want;
#else
        warnUnavailable( want );
        return platformDefaultBackend();
#endif
    }
    if( want == QLatin1String( "libsecret" ) ) {
#if defined( TW_HAVE_LIBSECRET )
        return want;
#else
        warnUnavailable( want );
        return platformDefaultBackend();
#endif
    }
    if( want == QLatin1String( "default" ) ) return platformDefaultBackend();

    warnUnknown( want );
    return platformDefaultBackend();
}

bool SSecretStore::canPersist() const
{
    return backend_ != QLatin1String( "none" );
}

QString SSecretStore::schemeKey_( const QString &key ) const
{
    return key + QStringLiteral( "/passwordScheme" );
}

QString SSecretStore::encKey_( const QString &key ) const
{
    return key + QStringLiteral( "/passwordEnc" );
}

bool SSecretStore::store( const QString &key, const QString &secret )
{
    if( backend_ == QLatin1String( "none" ) ) {
        // AC 3a: reports the failure plainly rather than appearing to
        // succeed and losing the secret at exit. No INI key, no keychain
        // item -- nothing below this line runs.
        TW_LOGI( "secretstore",
                 "no real credential store on this platform/build; '%s' is "
                 "NOT remembered past this session",
                 key.toUtf8().constData() );
        return false;
    }

    if( backend_ == QLatin1String( "memory" ) ) {
        QMutexLocker lock( &g_memoryMutex );
        g_memoryStore.insert( memoryMapKey( service_, key ), secret );
        return true;
    }

    if( settings_ == nullptr ) {
        // Misuse, not a data-path failure: a real backend was resolved
        // without a QSettings instance to write into. Never a Q_ASSERT here
        // (this build compiles those out) -- fail loudly and refuse, exactly
        // as an encryption failure below does.
        TW_LOGE( "secretstore",
                 "backend '%s' needs a QSettings instance and none was given; "
                 "'%s' was NOT stored",
                 backend_.toUtf8().constData(), key.toUtf8().constData() );
        return false;
    }

    const QByteArray plain = secret.toUtf8();

    if( backend_ == QLatin1String( "dpapi" ) ) {
#if defined( TW_HAVE_DPAPI )
        QByteArray cipher;
        if( !tw_secretstore_win::dpapiEncrypt( plain, &cipher ) ) {
            TW_LOGW( "secretstore", "DPAPI encryption failed for '%s'; not stored",
                     key.toUtf8().constData() );
            return false;
        }
        settings_->setValue( schemeKey_( key ), backend_ );
        settings_->setValue( encKey_( key ), QString::fromLatin1( cipher.toBase64() ) );
        settings_->sync();
        return true;
#else
        TW_LOGE( "secretstore", "resolveBackend() returned 'dpapi' but TW_HAVE_DPAPI is not compiled" );
        return false;
#endif
    }

    if( backend_ == QLatin1String( "keychain" ) ) {
#if defined( TW_HAVE_KEYCHAIN )
        if( !tw_secretstore_mac::keychainStore( service_, key, plain ) ) {
            TW_LOGW( "secretstore", "Keychain store failed for '%s'; not stored",
                     key.toUtf8().constData() );
            return false;
        }
        settings_->setValue( schemeKey_( key ), backend_ );
        settings_->sync();
        return true;
#else
        TW_LOGE( "secretstore", "resolveBackend() returned 'keychain' but TW_HAVE_KEYCHAIN is not compiled" );
        return false;
#endif
    }

    if( backend_ == QLatin1String( "libsecret" ) ) {
#if defined( TW_HAVE_LIBSECRET )
        if( !tw_secretstore_linux::libsecretStore( service_, key, plain ) ) {
            TW_LOGW( "secretstore", "libsecret store failed for '%s'; not stored",
                     key.toUtf8().constData() );
            return false;
        }
        settings_->setValue( schemeKey_( key ), backend_ );
        settings_->sync();
        return true;
#else
        TW_LOGE( "secretstore", "resolveBackend() returned 'libsecret' but TW_HAVE_LIBSECRET is not compiled" );
        return false;
#endif
    }

    TW_LOGE( "secretstore", "unreachable backend_ = '%s'", backend_.toUtf8().constData() );
    return false;
}

SSecretStore::Result SSecretStore::retrieve( const QString &key, QString *secret ) const
{
    if( backend_ == QLatin1String( "none" ) ) return Result::Unset;

    if( backend_ == QLatin1String( "memory" ) ) {
        QMutexLocker lock( &g_memoryMutex );
        auto it = g_memoryStore.constFind( memoryMapKey( service_, key ) );
        if( it == g_memoryStore.constEnd() ) return Result::Unset;
        *secret = it.value();
        return Result::Ok;
    }

    if( settings_ == nullptr ) {
        TW_LOGE( "secretstore",
                 "backend '%s' needs a QSettings instance and none was given; "
                 "'%s' reported as Unset",
                 backend_.toUtf8().constData(), key.toUtf8().constData() );
        return Result::Unset;
    }

    // Rule 1: the scheme tag stored AT THIS KEY governs how it is read back
    // -- never this instance's own currently-resolved backend_. That is what
    // makes a blob copied to another machine, or a scheme this build does
    // not have, fail as Undecryptable instead of being silently retried
    // under a different scheme (§B.8 rule 1, T14; AC 5).
    if( !settings_->contains( schemeKey_( key ) ) ) return Result::Unset;
    const QString scheme = settings_->value( schemeKey_( key ) ).toString();

    if( scheme == QLatin1String( "dpapi" ) ) {
#if defined( TW_HAVE_DPAPI )
        if( !settings_->contains( encKey_( key ) ) ) {
            TW_LOGW( "secretstore", "'%s' is tagged 'dpapi' with no ciphertext present",
                     key.toUtf8().constData() );
            return Result::Undecryptable;
        }
        const QByteArray cipher =
            QByteArray::fromBase64( settings_->value( encKey_( key ) ).toString().toLatin1() );
        QByteArray plain;
        if( cipher.isEmpty() || !tw_secretstore_win::dpapiDecrypt( cipher, &plain ) ) {
            TW_LOGW( "secretstore",
                     "'%s' (scheme=dpapi) could not be decrypted -- corrupt, or "
                     "written by a different user/machine",
                     key.toUtf8().constData() );
            return Result::Undecryptable;
        }
        *secret = QString::fromUtf8( plain );
        return Result::Ok;
#else
        TW_LOGW( "secretstore", "'%s' is tagged 'dpapi' but this build has no DPAPI backend",
                 key.toUtf8().constData() );
        return Result::Undecryptable;
#endif
    }

    if( scheme == QLatin1String( "keychain" ) ) {
#if defined( TW_HAVE_KEYCHAIN )
        QByteArray plain;
        if( !tw_secretstore_mac::keychainRetrieve( service_, key, &plain ) ) {
            TW_LOGW( "secretstore", "'%s' is tagged 'keychain' but no matching item was found",
                     key.toUtf8().constData() );
            return Result::Undecryptable;
        }
        *secret = QString::fromUtf8( plain );
        return Result::Ok;
#else
        TW_LOGW( "secretstore", "'%s' is tagged 'keychain' but this build has no Keychain backend",
                 key.toUtf8().constData() );
        return Result::Undecryptable;
#endif
    }

    if( scheme == QLatin1String( "libsecret" ) ) {
#if defined( TW_HAVE_LIBSECRET )
        QByteArray plain;
        if( !tw_secretstore_linux::libsecretRetrieve( service_, key, &plain ) ) {
            TW_LOGW( "secretstore", "'%s' is tagged 'libsecret' but no matching item was found",
                     key.toUtf8().constData() );
            return Result::Undecryptable;
        }
        *secret = QString::fromUtf8( plain );
        return Result::Ok;
#else
        TW_LOGW( "secretstore", "'%s' is tagged 'libsecret' but this build has no libsecret backend",
                 key.toUtf8().constData() );
        return Result::Undecryptable;
#endif
    }

    TW_LOGW( "secretstore", "'%s' has an unrecognised scheme tag '%s'",
             key.toUtf8().constData(), scheme.toUtf8().constData() );
    return Result::Undecryptable;
}

void SSecretStore::remove( const QString &key )
{
    if( backend_ == QLatin1String( "none" ) ) return;

    if( backend_ == QLatin1String( "memory" ) ) {
        QMutexLocker lock( &g_memoryMutex );
        g_memoryStore.remove( memoryMapKey( service_, key ) );
        return;
    }

    if( settings_ == nullptr ) return;

    const QString scheme = settings_->value( schemeKey_( key ) ).toString();
#if defined( TW_HAVE_KEYCHAIN )
    if( scheme == QLatin1String( "keychain" ) )
        tw_secretstore_mac::keychainRemove( service_, key );
#endif
#if defined( TW_HAVE_LIBSECRET )
    if( scheme == QLatin1String( "libsecret" ) )
        tw_secretstore_linux::libsecretRemove( service_, key );
#endif
    settings_->remove( schemeKey_( key ) );
    settings_->remove( encKey_( key ) );
    settings_->sync();
}
