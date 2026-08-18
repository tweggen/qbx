#ifndef _SSECRETSTOREBACKEND_H_
#define _SSECRETSTOREBACKEND_H_

// Private seam between ssecretstore.cpp's dispatcher and the per-platform
// backend translation units (ssecretstore_win.cpp / _mac.mm / _linux.cpp).
// Not a public header: nothing outside this directory may know a backend
// exists by this name -- app/shell/ssecretstore.h is the only public surface.
//
// TW_HAVE_DPAPI / TW_HAVE_KEYCHAIN / TW_HAVE_LIBSECRET are compile
// definitions set by main/CMakeLists.txt (mirroring TW_HAVE_CLAP /
// TW_HAVE_VST3 / TW_HAVE_AU in tw303a/CMakeLists.txt): TW_HAVE_DPAPI and
// TW_HAVE_KEYCHAIN follow the platform unconditionally (crypt32 and
// Security.framework are always present on their platforms -- no new
// dependency), TW_HAVE_LIBSECRET follows whether the optional libsecret-1
// pkg-config module was actually found at configure time.

#include <QByteArray>
#include <QString>

#if defined( TW_HAVE_DPAPI )
namespace tw_secretstore_win {

// Raw DPAPI encrypt/decrypt of an opaque byte buffer (never a C string --
// the secret may contain embedded NULs). `cipher`/`plain` are ONLY valid on a
// `true` return; a `false` return means "not handled", not "handled as
// empty" -- ssecretstore.cpp is the one place that turns that into
// Result::Undecryptable, with the key name in the log line.
bool dpapiEncrypt( const QByteArray &plain, QByteArray *cipher );
bool dpapiDecrypt( const QByteArray &cipher, QByteArray *plain );

} // namespace tw_secretstore_win
#endif

#if defined( TW_HAVE_KEYCHAIN )
namespace tw_secretstore_mac {

// `service` scopes the whole store (real: "com.smaragd.media"; a test: its
// own name -- AC 6a); `account` is the caller's opaque key. A missing item on
// retrieve/remove is reported as `false`, not logged as an error -- the
// caller decides whether "nothing there" is expected.
bool keychainStore( const QString &service, const QString &account, const QByteArray &secret );
bool keychainRetrieve( const QString &service, const QString &account, QByteArray *secret );
bool keychainRemove( const QString &service, const QString &account );

} // namespace tw_secretstore_mac
#endif

#if defined( TW_HAVE_LIBSECRET )
namespace tw_secretstore_linux {

bool libsecretStore( const QString &service, const QString &account, const QByteArray &secret );
bool libsecretRetrieve( const QString &service, const QString &account, QByteArray *secret );
bool libsecretRemove( const QString &service, const QString &account );

} // namespace tw_secretstore_linux
#endif

#endif
