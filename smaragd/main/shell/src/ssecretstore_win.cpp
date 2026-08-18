// Windows DPAPI backend for SSecretStore (proposal 38 §B.8). Only compiled
// when TW_HAVE_DPAPI is set (main/CMakeLists.txt, SMARAGD_WINDOWS). Links
// crypt32, which ships with MinGW -- no new dependency.
//
// CRYPTPROTECT_UI_FORBIDDEN on every call: a headless run must never hang on
// a credential-UI prompt nobody can see, exactly the qoffscreen lesson
// (CLAUDE.md) applied to DPAPI rather than to a Qt platform plugin.

#include "ssecretstorebackend.h"

#if defined( TW_HAVE_DPAPI )

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <wincrypt.h>

#include "tw/core/twlog.h"

namespace tw_secretstore_win {

bool dpapiEncrypt( const QByteArray &plain, QByteArray *cipher )
{
    if( cipher == nullptr ) return false;

    DATA_BLOB in{};
    in.pbData = reinterpret_cast<BYTE *>( const_cast<char *>( plain.constData() ) );
    in.cbData = static_cast<DWORD>( plain.size() );

    DATA_BLOB out{};
    // The description string is diagnostic only (surfaces in some Windows
    // credential UIs); it is not part of the key derivation. User-scoped by
    // default -- no CRYPTPROTECT_LOCAL_MACHINE flag -- so the key comes from
    // the logon credential and stays put even if another account is added to
    // the same machine.
    const BOOL ok = CryptProtectData( &in, L"Smaragd secret", nullptr, nullptr, nullptr,
                                       CRYPTPROTECT_UI_FORBIDDEN, &out );
    if( !ok ) {
        TW_LOGW( "secretstore", "DPAPI CryptProtectData failed (error %lu)", GetLastError() );
        return false;
    }

    *cipher = QByteArray( reinterpret_cast<const char *>( out.pbData ), static_cast<int>( out.cbData ) );
    // The plaintext momentarily sat in this heap block too (CryptProtectData
    // reads it directly from `in`, but out.pbData is CryptProtectData's own
    // allocation and does not need zeroing for that reason -- kept anyway as
    // the same discipline the decrypt side needs for its OWN plaintext copy).
    LocalFree( out.pbData );
    return true;
}

bool dpapiDecrypt( const QByteArray &cipher, QByteArray *plain )
{
    if( plain == nullptr || cipher.isEmpty() ) return false;

    DATA_BLOB in{};
    in.pbData = reinterpret_cast<BYTE *>( const_cast<char *>( cipher.constData() ) );
    in.cbData = static_cast<DWORD>( cipher.size() );

    DATA_BLOB out{};
    const BOOL ok = CryptUnprotectData( &in, nullptr, nullptr, nullptr, nullptr,
                                         CRYPTPROTECT_UI_FORBIDDEN, &out );
    if( !ok ) {
        // Deliberately no GetLastError() text logged here -- this path is
        // reached by ordinary corrupt/foreign-machine ciphertext, not just
        // programming bugs, and the caller (ssecretstore.cpp) already logs
        // which KEY failed. A `false` return carries no secret.
        return false;
    }

    *plain = QByteArray( reinterpret_cast<const char *>( out.pbData ), static_cast<int>( out.cbData ) );
    // out.pbData held the recovered PLAINTEXT secret. Zero it before freeing
    // -- the one place in this backend where a real password sits in a raw
    // heap block under our control.
    SecureZeroMemory( out.pbData, out.cbData );
    LocalFree( out.pbData );
    return true;
}

} // namespace tw_secretstore_win

#endif // TW_HAVE_DPAPI
