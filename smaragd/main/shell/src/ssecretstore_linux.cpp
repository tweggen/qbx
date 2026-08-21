// Linux libsecret backend for SSecretStore (proposal 38 §B.8). Only compiled
// when TW_HAVE_LIBSECRET is set (main/CMakeLists.txt) -- an OPTIONAL
// dependency: this file is not even added to the build when libsecret-1 was
// not found at configure time, exactly the TW_HAVE_CLAP discipline. Without
// it, `resolveBackend()`'s platform default on Linux is `none`.
//
// UNCOMPILED AND UNTESTED on the box this gate was built and gated on
// (Windows/MinGW). Written to the documented libsecret synchronous API and
// reviewed against it; no headless run can exercise it (Secret Service needs
// a running session bus / keyring daemon).

// libsecret (and the glib it drags in) MUST be included BEFORE any Qt header.
// Qt's qobjectdefs.h does `#define signals public`, and glib's
// gdbusintrospection.h declares a MEMBER named `signals` -- so with Qt first
// that member becomes `GDBusSignalInfo **public;` and the compile dies inside
// a system header, pointing at glib rather than at us. TW_HAVE_LIBSECRET is a
// compile definition from CMake, so it is already usable up here, above the
// include that pulls Qt in.
#if defined( TW_HAVE_LIBSECRET )
#include <libsecret/secret.h>
#endif

#include "ssecretstorebackend.h"

#if defined( TW_HAVE_LIBSECRET )

#include "tw/core/twlog.h"

namespace tw_secretstore_linux {

namespace {

const SecretSchema *schema()
{
    static const SecretSchema kSchema = {
        "com.smaragd.SecretStore", SECRET_SCHEMA_NONE,
        {
            { "service", SECRET_SCHEMA_ATTRIBUTE_STRING },
            { "account", SECRET_SCHEMA_ATTRIBUTE_STRING },
            { nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING },
        }
    };
    return &kSchema;
}

} // namespace

bool libsecretStore( const QString &service, const QString &account, const QByteArray &secret )
{
    GError *error = nullptr;
    // libsecret's store call wants a NUL-terminated UTF-8 "password" C
    // string, not an arbitrary byte buffer -- an embedded NUL in the secret
    // would silently truncate at the libsecret boundary. Base64-encoding
    // first keeps what actually reaches libsecret ASCII-safe and NUL-free;
    // AC 2's byte-exactness is then entirely our own base64 layer's
    // responsibility, which is already exercised (and gated) via the dpapi
    // path.
    const QByteArray encoded = secret.toBase64();
    const QByteArray label   = QStringLiteral( "Smaragd: %1" ).arg( service ).toUtf8();

    const gboolean ok = secret_password_store_sync(
        schema(), SECRET_COLLECTION_DEFAULT, label.constData(), encoded.constData(), nullptr, &error,
        "service", service.toUtf8().constData(), "account", account.toUtf8().constData(), nullptr );

    if( !ok || error != nullptr ) {
        TW_LOGW( "secretstore", "libsecret store failed for service='%s': %s",
                 service.toUtf8().constData(), error ? error->message : "unknown error" );
        if( error ) g_error_free( error );
        return false;
    }
    return true;
}

bool libsecretRetrieve( const QString &service, const QString &account, QByteArray *secret )
{
    GError *error   = nullptr;
    gchar  *encoded = secret_password_lookup_sync(
        schema(), nullptr, &error, "service", service.toUtf8().constData(), "account",
        account.toUtf8().constData(), nullptr );

    if( error != nullptr ) {
        TW_LOGW( "secretstore", "libsecret lookup failed for service='%s': %s",
                 service.toUtf8().constData(), error->message );
        g_error_free( error );
        return false;
    }
    if( encoded == nullptr ) return false; // not found -- not an error

    *secret = QByteArray::fromBase64( QByteArray( encoded ) );
    secret_password_free( encoded );
    return true;
}

bool libsecretRemove( const QString &service, const QString &account )
{
    GError *error = nullptr;
    // Return value intentionally unused: whether an item existed to clear is
    // not this call's business, only whether the OPERATION errored.
    secret_password_clear_sync( schema(), nullptr, &error, "service", service.toUtf8().constData(),
                                 "account", account.toUtf8().constData(), nullptr );

    if( error != nullptr ) {
        TW_LOGW( "secretstore", "libsecret clear failed for service='%s': %s",
                 service.toUtf8().constData(), error->message );
        g_error_free( error );
        return false;
    }
    return true;
}

} // namespace tw_secretstore_linux

#endif // TW_HAVE_LIBSECRET
