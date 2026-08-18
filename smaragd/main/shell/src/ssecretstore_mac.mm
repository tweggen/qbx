// macOS Keychain backend for SSecretStore (proposal 38 §B.8). Only compiled
// when TW_HAVE_KEYCHAIN is set (main/CMakeLists.txt, SMARAGD_MACOS). Links
// Security.framework -- ships in the SDK, no new dependency. Plain C Security
// API (no Objective-C runtime needed for it), matching the house style set by
// twaumodule.cc/twauplugin.cc (plain C AudioUnit, no Obj-C) even though this
// file is compiled as Objective-C++ (.mm) because CFString/CFDictionary
// bridging is the idiomatic way to build a SecItem query.
//
// UNCOMPILED AND UNTESTED on the box this gate was built and gated on
// (Windows/MinGW). Written to the documented Security.framework contract and
// reviewed against it; the manual macOS run (proposal 38 §C.6 / gate 6) is
// what actually exercises it.

#include "ssecretstorebackend.h"

#if defined( TW_HAVE_KEYCHAIN )

#include <Security/Security.h>

#include "tw/core/twlog.h"

namespace tw_secretstore_mac {

namespace {

CFStringRef toCFString( const QString &s )
{
    const QByteArray utf8 = s.toUtf8();
    return CFStringCreateWithBytes( kCFAllocatorDefault,
                                     reinterpret_cast<const UInt8 *>( utf8.constData() ),
                                     utf8.size(), kCFStringEncodingUTF8, false );
}

// Builds the base query dictionary shared by store/retrieve/remove. Caller
// releases the returned dictionary and the two CFStrings it retained.
CFMutableDictionaryRef baseQuery( const QString &service, const QString &account,
                                   CFStringRef *outService, CFStringRef *outAccount )
{
    CFStringRef cfService = toCFString( service );
    CFStringRef cfAccount = toCFString( account );

    CFMutableDictionaryRef query = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks );
    CFDictionarySetValue( query, kSecClass, kSecClassGenericPassword );
    CFDictionarySetValue( query, kSecAttrService, cfService );
    CFDictionarySetValue( query, kSecAttrAccount, cfAccount );
    // Never sync a locally-managed app credential to iCloud Keychain.
    CFDictionarySetValue( query, kSecAttrSynchronizable, kCFBooleanFalse );

    *outService = cfService;
    *outAccount = cfAccount;
    return query;
}

} // namespace

bool keychainStore( const QString &service, const QString &account, const QByteArray &secret )
{
    // Overwrite semantics: clear any existing item first so a re-store is
    // idempotent instead of failing with errSecDuplicateItem.
    keychainRemove( service, account );

    CFStringRef cfService = nullptr;
    CFStringRef cfAccount = nullptr;
    CFMutableDictionaryRef query = baseQuery( service, account, &cfService, &cfAccount );

    CFDataRef cfSecret = CFDataCreate( kCFAllocatorDefault,
                                        reinterpret_cast<const UInt8 *>( secret.constData() ),
                                        secret.size() );
    CFDictionarySetValue( query, kSecValueData, cfSecret );

    const OSStatus status = SecItemAdd( query, nullptr );

    CFRelease( cfSecret );
    CFRelease( query );
    CFRelease( cfAccount );
    CFRelease( cfService );

    if( status != errSecSuccess ) {
        TW_LOGW( "secretstore", "Keychain SecItemAdd failed for service='%s' (status %d)",
                 service.toUtf8().constData(), static_cast<int>( status ) );
        return false;
    }
    return true;
}

bool keychainRetrieve( const QString &service, const QString &account, QByteArray *secret )
{
    CFStringRef cfService = nullptr;
    CFStringRef cfAccount = nullptr;
    CFMutableDictionaryRef query = baseQuery( service, account, &cfService, &cfAccount );
    CFDictionarySetValue( query, kSecReturnData, kCFBooleanTrue );
    CFDictionarySetValue( query, kSecMatchLimit, kSecMatchLimitOne );

    CFTypeRef result = nullptr;
    const OSStatus status = SecItemCopyMatching( query, &result );

    CFRelease( query );
    CFRelease( cfAccount );
    CFRelease( cfService );

    if( status != errSecSuccess || result == nullptr ) {
        if( status != errSecItemNotFound ) {
            TW_LOGW( "secretstore", "Keychain SecItemCopyMatching failed for service='%s' (status %d)",
                     service.toUtf8().constData(), static_cast<int>( status ) );
        }
        return false;
    }

    CFDataRef data = static_cast<CFDataRef>( result );
    *secret = QByteArray( reinterpret_cast<const char *>( CFDataGetBytePtr( data ) ),
                           static_cast<int>( CFDataGetLength( data ) ) );
    CFRelease( result );
    return true;
}

bool keychainRemove( const QString &service, const QString &account )
{
    CFStringRef cfService = nullptr;
    CFStringRef cfAccount = nullptr;
    CFMutableDictionaryRef query = baseQuery( service, account, &cfService, &cfAccount );

    const OSStatus status = SecItemDelete( query );

    CFRelease( query );
    CFRelease( cfAccount );
    CFRelease( cfService );

    // errSecItemNotFound is not a failure here -- remove() is a no-op on
    // nothing, exactly like SSecretStore::remove()'s own contract.
    return status == errSecSuccess || status == errSecItemNotFound;
}

} // namespace tw_secretstore_mac

#endif // TW_HAVE_KEYCHAIN
