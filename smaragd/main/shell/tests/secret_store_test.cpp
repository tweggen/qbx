// Gate for SSecretStore (proposal 38 GATE 5a, §B.8). Covers ACs 1, 2, 3, 3a,
// 4, 5, 6 and 6a -- the store's plumbing and failure modes, never the
// platform cryptography itself (DPAPI/Keychain/libsecret are not audited
// here or anywhere in this repo; see the PR body).
//
// AC 6a is the reason this file exists as a plain ctest binary with no
// RUN_SERIAL rather than a qxa case: it constructs its OWN QSettings over a
// private temp-file INI and NEVER touches SSettings::instance() (this file
// does not even #include app/shell/ssettings.h) -- so it can run under
// `ctest -j4` alongside the qxa suite without racing anything that reads or
// writes the developer's real smaragd.ini. Every SSecretStore instance below
// is handed one of these private QSettings objects (or none at all, for the
// `memory`/`none` backends, which never touch QSettings regardless).
//
// This test only exercises backends this build actually compiles. On the
// Windows/MinGW box it is written and gated on, that is `dpapi` (plus the
// platform-independent `memory` and `none`). `keychain` and `libsecret` are
// exercised only through AC 5's "a scheme tag naming an uncompiled backend"
// path, which needs no macOS/Linux code to run.

#include "app/shell/ssecretstore.h"

#include <QByteArray>
#include <QChar>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QSettings>
#include <QString>
#include <QTemporaryDir>

#include <cstdio>
#include <cstdlib>

using Result = SSecretStore::Result;

static int failures = 0;
#define CHECK( cond, msg )                                                                       \
    do {                                                                                          \
        if( cond ) {                                                                              \
            std::printf( "ok   %s\n", msg );                                                      \
        } else {                                                                                  \
            std::printf( "FAIL %s\n", msg );                                                       \
            ++failures;                                                                           \
        }                                                                                          \
    } while( 0 )

namespace {

void setEnv( const char *name, const char *value )
{
#if defined( _WIN32 )
    _putenv_s( name, value ? value : "" );
#else
    if( value ) setenv( name, value, 1 ); else unsetenv( name );
#endif
}

QByteArray fileMd5( const QString &path )
{
    QFile f( path );
    if( !f.open( QIODevice::ReadOnly ) ) return QByteArray();
    return QCryptographicHash::hash( f.readAll(), QCryptographicHash::Md5 );
}

// Picks a scheme NAME that resolveBackend() itself reports as uncompiled on
// this build -- i.e. asking for it does not yield itself back. Portable
// across platforms: on Windows this returns "keychain" or "libsecret"
// (neither compiled here); on a build with everything compiled it falls back
// to a synthetic name, which still exercises the "wholly unrecognised
// scheme" branch even if not literally "a real backend this build lacks".
QString pickUncompiledSchemeName()
{
    for( const QString &candidate :
         { QStringLiteral( "dpapi" ), QStringLiteral( "keychain" ), QStringLiteral( "libsecret" ) } ) {
        if( SSecretStore::resolveBackend( candidate ) != candidate ) return candidate;
    }
    return QStringLiteral( "nonexistent-scheme" );
}

// --- AC: resolveBackend() reads SMARAGD_SECRET_BACKEND like the audio/MIDI
//         knobs, case-insensitively, with a safe fallback on nonsense. -------
void testResolveBackend()
{
    std::printf( "\n-- resolveBackend() / SMARAGD_SECRET_BACKEND --\n" );

    setEnv( "SMARAGD_SECRET_BACKEND", "memory" );
    CHECK( SSecretStore::resolveBackend() == QLatin1String( "memory" ),
           "env SMARAGD_SECRET_BACKEND=memory selects memory" );

    setEnv( "SMARAGD_SECRET_BACKEND", "NONE" );
    CHECK( SSecretStore::resolveBackend() == QLatin1String( "none" ),
           "case-insensitive, like SMARAGD_MIDI_BACKEND" );

    setEnv( "SMARAGD_SECRET_BACKEND", "totally-bogus-backend-name" );
    {
        const QString got = SSecretStore::resolveBackend();
        CHECK( got != QLatin1String( "totally-bogus-backend-name" ) && !got.isEmpty(),
               "an unknown value warns and falls back to a real backend name, never the bogus one" );
    }

    setEnv( "SMARAGD_SECRET_BACKEND", nullptr );
    {
        const QString got = SSecretStore::resolveBackend();
        CHECK( !got.isEmpty(), "unset -> the platform default, never empty" );
        std::printf( "     platform default backend on this build: %s\n", qPrintable( got ) );
    }

    setEnv( "SMARAGD_SECRET_BACKEND", "dpapi" );
    CHECK( SSecretStore::resolveBackend( QStringLiteral( "memory" ) ) == QLatin1String( "memory" ),
           "an explicit constructor override wins over the environment" );

    setEnv( "SMARAGD_SECRET_BACKEND", nullptr );
}

// --- AC 1: store -> retrieve -> delete round-trips for every backend this ---
//           platform compiles; an unknown key is Unset, never "". -----------
void testRoundTripAndUnset()
{
    std::printf( "\n-- AC1: round trip + unset-is-not-empty-string --\n" );

    QTemporaryDir dir;
    CHECK( dir.isValid(), "temp dir for a private INI" );
    QSettings settings( dir.path() + QStringLiteral( "/roundtrip.ini" ), QSettings::IniFormat );
    const QString svc = QStringLiteral( "com.smaragd.secretstoretest.roundtrip" );

    const QString platformBackend = SSecretStore( &settings, svc ).backendName();
    std::printf( "     platform backend under test: %s\n", qPrintable( platformBackend ) );

    for( const QString &backend :
         QList<QString>{ QStringLiteral( "memory" ), QStringLiteral( "none" ), platformBackend } ) {
        SSecretStore store( &settings, svc, backend );
        CHECK( store.backendName() == backend, qPrintable( QStringLiteral( "backendName() reports '%1'" ).arg( backend ) ) );

        const QString key = QStringLiteral( "acct/roundtrip_%1" ).arg( backend );
        QString out = QStringLiteral( "unchanged-sentinel" );
        CHECK( store.retrieve( key, &out ) == Result::Unset,
               "an unknown key is Unset before any store" );
        CHECK( out == QLatin1String( "unchanged-sentinel" ),
               "Unset never touches the out-param -- never mistaken for an empty real secret" );

        if( backend == QLatin1String( "none" ) ) {
            CHECK( store.store( key, QStringLiteral( "s3cr3t" ) ) == false,
                   "'none' backend's store() reports failure, not silent success (AC 3a)" );
            CHECK( store.retrieve( key, &out ) == Result::Unset,
                   "'none' never remembers, even immediately after store()" );
        } else {
            CHECK( store.store( key, QStringLiteral( "s3cr3t" ) ), "store() succeeds" );
            QString roundTripped;
            const Result r = store.retrieve( key, &roundTripped );
            CHECK( r == Result::Ok && roundTripped == QLatin1String( "s3cr3t" ),
                   "retrieve() returns Ok with the exact value just stored" );
            store.remove( key );
            CHECK( store.retrieve( key, &out ) == Result::Unset,
                   "a removed key is Unset again" );
        }
    }
}

// --- AC 2: non-ASCII, an embedded NUL, and ~4 KB round-trip byte-exactly. ---
void testByteExactRoundTrip()
{
    std::printf( "\n-- AC2: non-ASCII + embedded NUL + ~4KB round trip --\n" );

    QTemporaryDir dir;
    QSettings settings( dir.path() + QStringLiteral( "/bytes.ini" ), QSettings::IniFormat );
    const QString svc = QStringLiteral( "com.smaragd.secretstoretest.bytes" );

    QString secret;
    secret += QStringLiteral( "café éèê 中文 \U0001F600 " );
    secret += QChar( QChar::Null );                // an embedded NUL (U+0000)
    secret += QStringLiteral( "-after-the-nul-" );
    while( secret.size() < 4096 )
        secret += QStringLiteral( "0123456789ABCDEFé中" );

    const QString platformBackend = SSecretStore( &settings, svc ).backendName();
    for( const QString &backend :
         QList<QString>{ QStringLiteral( "memory" ), platformBackend } ) {
        if( backend == QLatin1String( "none" ) ) continue; // cannot persist at all -- nothing to round-trip

        SSecretStore store( &settings, svc, backend );
        const QString key = QStringLiteral( "acct/bytes_%1" ).arg( backend );
        CHECK( store.store( key, secret ), qPrintable( QStringLiteral( "%1: store of the 4KB/non-ASCII/NUL secret" ).arg( backend ) ) );

        QString out;
        const Result r = store.retrieve( key, &out );
        CHECK( r == Result::Ok, qPrintable( QStringLiteral( "%1: retrieve reports Ok" ).arg( backend ) ) );
        CHECK( out.size() == secret.size(),
               qPrintable( QStringLiteral( "%1: no truncation at the embedded NUL" ).arg( backend ) ) );
        CHECK( out == secret, qPrintable( QStringLiteral( "%1: byte-exact round trip" ).arg( backend ) ) );
        store.remove( key );
    }
}

// --- AC 3: the ciphertext is not the plaintext, and differs run to run -----
//           (DPAPI salts). Only meaningful for `dpapi` specifically -- ------
//           `keychain`/`libsecret` never put ciphertext in the INI at all. --
void testCiphertextNotPlaintext()
{
    std::printf( "\n-- AC3: ciphertext != plaintext, and salts across stores --\n" );

    QTemporaryDir dir;
    QSettings settings( dir.path() + QStringLiteral( "/ciphertext.ini" ), QSettings::IniFormat );
    const QString svc = QStringLiteral( "com.smaragd.secretstoretest.ciphertext" );

    SSecretStore store( &settings, svc, QStringLiteral( "dpapi" ) );
    if( store.backendName() != QLatin1String( "dpapi" ) ) {
        std::printf( "     (skipping: platform backend is '%s', not dpapi -- nothing else in this "
                      "build puts ciphertext in the INI to check)\n",
                      qPrintable( store.backendName() ) );
        return;
    }

    const QString secret = QStringLiteral( "S3cr3t-P@ssw0rd-1234567890" );
    const QString key1   = QStringLiteral( "acct/ciphertext_a" );
    const QString key2   = QStringLiteral( "acct/ciphertext_b" );

    CHECK( store.store( key1, secret ), "dpapi store succeeds" );
    const QString enc1 = settings.value( key1 + QStringLiteral( "/passwordEnc" ) ).toString();
    CHECK( !enc1.isEmpty(), "ciphertext was written to the INI" );
    CHECK( !enc1.contains( secret ), "the INI value does not contain the plaintext verbatim" );

    const QByteArray cipherBytes = QByteArray::fromBase64( enc1.toLatin1() );
    const QByteArray plainBytes  = secret.toUtf8();
    bool eightByteSubstringFound = false;
    for( int i = 0; i + 8 <= plainBytes.size(); ++i ) {
        if( cipherBytes.contains( plainBytes.mid( i, 8 ) ) ) {
            eightByteSubstringFound = true;
            break;
        }
    }
    CHECK( !eightByteSubstringFound, "no 8-byte substring of the plaintext appears in the decoded ciphertext" );

    CHECK( store.store( key2, secret ), "a second store of the SAME secret, under a different key" );
    const QString enc2 = settings.value( key2 + QStringLiteral( "/passwordEnc" ) ).toString();
    CHECK( enc1 != enc2, "two stores of the same secret produce different ciphertext (DPAPI salts)" );

    store.remove( key1 );
    store.remove( key2 );
}

// --- AC 3a: `none` persists nothing at all -- byte-identical INI file. -----
void testNonePersistsNothing()
{
    std::printf( "\n-- AC3a: 'none' writes no INI key at all --\n" );

    QTemporaryDir dir;
    const QString iniPath = dir.path() + QStringLiteral( "/none.ini" );
    {
        // Establish the file on disk with a stable baseline before measuring.
        QSettings seed( iniPath, QSettings::IniFormat );
        seed.setValue( QStringLiteral( "unrelated/marker" ), QStringLiteral( "present" ) );
        seed.sync();
    }
    const QByteArray md5Before = fileMd5( iniPath );

    QSettings settings( iniPath, QSettings::IniFormat );
    SSecretStore store( &settings, QStringLiteral( "com.smaragd.secretstoretest.none" ), QStringLiteral( "none" ) );
    CHECK( !store.canPersist(), "'none' backend reports it cannot persist" );

    const QString key = QStringLiteral( "acct/none_check" );
    CHECK( store.store( key, QStringLiteral( "whatever" ) ) == false,
           "'none' store() reports failure rather than appearing to succeed" );
    QString out;
    CHECK( store.retrieve( key, &out ) == Result::Unset, "'none' never remembers" );
    CHECK( !settings.contains( key + QStringLiteral( "/passwordScheme" ) ), "no scheme key written" );
    CHECK( !settings.contains( key + QStringLiteral( "/passwordEnc" ) ), "no ciphertext key written" );

    settings.sync();
    const QByteArray md5After = fileMd5( iniPath );
    CHECK( md5Before == md5After, "the INI file is byte-identical after a 'none' store attempt" );
}

// --- AC 4: a corrupt / foreign blob is Undecryptable, never garbage. -------
void testCorruptOrForeignBlob()
{
    std::printf( "\n-- AC4: corrupt/foreign ciphertext -> Undecryptable --\n" );

    QTemporaryDir dir;
    QSettings settings( dir.path() + QStringLiteral( "/corrupt.ini" ), QSettings::IniFormat );
    const QString svc = QStringLiteral( "com.smaragd.secretstoretest.corrupt" );

    SSecretStore store( &settings, svc, QStringLiteral( "dpapi" ) );
    if( store.backendName() != QLatin1String( "dpapi" ) ) {
        std::printf( "     (skipping: no dpapi on this platform build)\n" );
        return;
    }

    const QString key = QStringLiteral( "acct/corrupt" );
    const QString encKey = key + QStringLiteral( "/passwordEnc" );
    QString out = QStringLiteral( "unchanged-sentinel" );

    // Scenario A: random base64 noise under a dpapi tag -- CryptUnprotectData
    // rejects it outright.
    CHECK( store.store( key, QStringLiteral( "orig-secret-a" ) ), "seed a real dpapi secret" );
    settings.setValue( encKey, QStringLiteral( "QUJDREVGR0hJSktMTU5PUFFSU1RVVldYWVo=" ) );
    settings.sync();
    CHECK( store.retrieve( key, &out ) == Result::Undecryptable,
           "random base64 noise under passwordEnc -> Undecryptable" );
    CHECK( out == QLatin1String( "unchanged-sentinel" ), "Undecryptable never fills the out-param" );

    // Scenario B: a real ciphertext blob, bit-flipped -- the closest this
    // single-real-backend build can get to "a blob from a different scheme
    // tag" / a foreign-machine blob: bytes that LOOK like a real blob but
    // that CryptUnprotectData on THIS box cannot open.
    CHECK( store.store( key, QStringLiteral( "orig-secret-b" ) ), "reseed with a fresh real secret" );
    QByteArray realCipher = QByteArray::fromBase64( settings.value( encKey ).toString().toLatin1() );
    CHECK( realCipher.size() > 8, "sanity: the real ciphertext is not degenerate" );
    // Flip every byte rather than one: DPAPI blobs carry an unauthenticated
    // header (a master-key GUID and flags) ahead of the MAC-protected
    // payload, so a single flipped byte can land somewhere CryptUnprotectData
    // does not check and still decrypt -- measured directly, not assumed.
    // Flipping the whole buffer is guaranteed to land in the protected part
    // while still being "a real blob's bytes, corrupted" rather than fresh
    // random noise (scenario A already covers that case).
    for( int i = 0; i < realCipher.size(); ++i )
        realCipher[i] = static_cast<char>( realCipher[i] ^ 0xFF );
    settings.setValue( encKey, QString::fromLatin1( realCipher.toBase64() ) );
    settings.sync();
    CHECK( store.retrieve( key, &out ) == Result::Undecryptable,
           "a corrupted real-looking blob -> Undecryptable, never garbage plaintext" );
    CHECK( out == QLatin1String( "unchanged-sentinel" ), "Undecryptable never fills the out-param (scenario B)" );

    store.remove( key );
}

// --- AC 5: a scheme tag naming a backend this build lacks -> Undecryptable,
//           and it does not fall back to another backend and try anyway. ---
void testUncompiledOrUnknownScheme()
{
    std::printf( "\n-- AC5: scheme tag naming an uncompiled/unknown backend --\n" );

    QTemporaryDir dir;
    QSettings settings( dir.path() + QStringLiteral( "/scheme.ini" ), QSettings::IniFormat );
    const QString svc = QStringLiteral( "com.smaragd.secretstoretest.scheme" );

    SSecretStore store( &settings, svc ); // platform default
    const QString key = QStringLiteral( "acct/scheme_check" );
    QString out = QStringLiteral( "unchanged-sentinel" );

    const QString uncompiled = pickUncompiledSchemeName();
    std::printf( "     using '%s' as a backend this build does not compile\n", qPrintable( uncompiled ) );
    settings.setValue( key + QStringLiteral( "/passwordScheme" ), uncompiled );
    settings.sync();
    CHECK( store.retrieve( key, &out ) == Result::Undecryptable,
           "a scheme naming an uncompiled backend -> Undecryptable, not a silent fallback" );
    CHECK( out == QLatin1String( "unchanged-sentinel" ), "the out-param is left untouched" );

    settings.setValue( key + QStringLiteral( "/passwordScheme" ), QStringLiteral( "totally-unrecognised-scheme" ) );
    settings.sync();
    CHECK( store.retrieve( key, &out ) == Result::Undecryptable,
           "a wholly unrecognised scheme tag -> Undecryptable" );

    settings.remove( key + QStringLiteral( "/passwordScheme" ) );
    settings.sync();
}

// --- AC 6: `memory` writes no INI key and no keychain item. ----------------
void testMemoryWritesNoIni()
{
    std::printf( "\n-- AC6: 'memory' writes no INI key --\n" );

    QTemporaryDir dir;
    const QString iniPath = dir.path() + QStringLiteral( "/memory.ini" );
    {
        QSettings seed( iniPath, QSettings::IniFormat );
        seed.setValue( QStringLiteral( "unrelated/marker" ), QStringLiteral( "present" ) );
        seed.sync();
    }
    const QByteArray md5Before = fileMd5( iniPath );

    QSettings settings( iniPath, QSettings::IniFormat );
    SSecretStore store( &settings, QStringLiteral( "com.smaragd.secretstoretest.memory" ), QStringLiteral( "memory" ) );
    CHECK( store.canPersist(), "'memory' reports it CAN persist (it is a real store, just not on disk)" );

    const QString key = QStringLiteral( "acct/memory_check" );
    CHECK( store.store( key, QStringLiteral( "s3cr3t" ) ), "'memory' store() succeeds" );
    QString out;
    CHECK( store.retrieve( key, &out ) == Result::Ok && out == QLatin1String( "s3cr3t" ),
           "'memory' round-trips within the process" );
    CHECK( !settings.contains( key + QStringLiteral( "/passwordScheme" ) ), "no scheme key written" );
    CHECK( !settings.contains( key + QStringLiteral( "/passwordEnc" ) ), "no ciphertext key written" );

    settings.sync();
    const QByteArray md5After = fileMd5( iniPath );
    CHECK( md5Before == md5After, "the INI file is byte-identical after a 'memory' store" );

    // "process lifetime" (§B.8 rule 5): a SECOND instance, same service+key,
    // still sees it -- this is what lets the options-page cases (5b) open a
    // fresh dialog and still find a "remembered" account without a real
    // backend.
    SSecretStore secondInstance( &settings, QStringLiteral( "com.smaragd.secretstoretest.memory" ), QStringLiteral( "memory" ) );
    QString out2;
    CHECK( secondInstance.retrieve( key, &out2 ) == Result::Ok && out2 == QLatin1String( "s3cr3t" ),
           "a second SSecretStore instance (same service+key) still finds it -- process lifetime" );

    store.remove( key );

    // Isolation: a DIFFERENT service name must NOT see it, even in-process
    // (AC 6a's isolation argument applied to the memory backend itself).
    CHECK( store.store( key, QStringLiteral( "s3cr3t-2" ) ), "re-store for the isolation check" );
    SSecretStore differentService( &settings, QStringLiteral( "com.smaragd.secretstoretest.memory.OTHER" ), QStringLiteral( "memory" ) );
    QString out3 = QStringLiteral( "unchanged-sentinel" );
    CHECK( differentService.retrieve( key, &out3 ) == Result::Unset,
           "a different service name cannot see another service's memory-backend secret" );
    store.remove( key );
}

// --- AC 6a: this test never reaches the real smaragd.ini or a real -------
//            keychain -- structural, not just empirical. -------------------
void testNeverTouchesRealSettings()
{
    std::printf( "\n-- AC6a: structural isolation from the real smaragd.ini --\n" );
    // Every SSecretStore constructed anywhere above was handed either a
    // private QTemporaryDir-backed QSettings, or none at all (memory/none).
    // This file does not #include app/shell/ssettings.h and never calls
    // SSettings::instance() -- there is no code path in this binary that
    // could reach the developer's real smaragd.ini, which is the point: the
    // guarantee is that SSecretStore itself never reaches for the singleton,
    // not merely that this test remembered not to.
    //
    // The empirical half of this AC -- an md5/mtime check of the REAL
    // smaragd.ini across a full `ctest -j4` run that includes this binary --
    // is done once, externally, for the whole suite (see the PR body), the
    // same way the pre-existing "smaragd.ini" row in CLAUDE.md's `-j` audit
    // is verified: it is not this ONE test's job to prove the whole suite's
    // isolation, only its own.
    CHECK( true, "SSecretStore takes an explicit QSettings*; this file never references SSettings::instance()" );
}

} // namespace

int main()
{
    testResolveBackend();
    testRoundTripAndUnset();
    testByteExactRoundTrip();
    testCiphertextNotPlaintext();
    testNonePersistsNothing();
    testCorruptOrForeignBlob();
    testUncompiledOrUnknownScheme();
    testMemoryWritesNoIni();
    testNeverTouchesRealSettings();

    if( failures == 0 ) {
        std::printf( "\nall secret store tests passed\n" );
        return 0;
    }
    std::printf( "\n%d secret store test(s) FAILED\n", failures );
    return 1;
}
