#ifndef _SSECRETSTORE_H_
#define _SSECRETSTORE_H_

#include <QString>

class QSettings;

// A credential store: stores and retrieves an opaque secret STRING by key
// name, encrypted at rest by whatever the platform actually offers (proposal
// 38 §B.8). Deliberately narrow -- "give me the bytes back for this key",
// nothing about accounts, URLs or HTTP. That is what lets a future bearer
// token (Nextcloud Login Flow v2, §B.8a) land in the same store as today's
// app password: the caller decides what the string MEANS, this class only
// protects it.
//
// | Backend     | Where the bytes live          | How protected                |
// |-------------|--------------------------------|-------------------------------|
// | `dpapi`     | ciphertext, base64, in the INI | CryptProtectData, user-scoped |
// | `keychain`  | the macOS login keychain       | Security.framework            |
// | `libsecret` | the Linux Secret Service       | libsecret (optional dep)      |
// | `none`      | nowhere                        | no real store on this build   |
// | `memory`    | a process-lifetime in-RAM map  | none -- the test backend      |
//
// There is deliberately NO "obfuscated"/encrypt-it-ourselves fallback: this
// tree links no cipher, and a scheme that would have to document itself as
// protecting nothing is a checkbox, not a control. A platform with no real
// store gets `none`: nothing is persisted, `store()` reports that plainly,
// and the caller is expected to keep the plaintext in memory for the session
// only (that policy lives in the caller/UI, not here).
//
// Five rules this class exists to uphold (proposal 38 §B.8):
//   1. The scheme tag is load-bearing. A blob is decrypted only through the
//      backend NAMED at the key it was written under -- never the instance's
//      own currently-selected backend -- so a blob copied to another machine,
//      or naming a backend this build does not have, fails READABLY
//      (Result::Undecryptable) instead of yielding garbage or silently
//      trying a different scheme.
//   2. A secret never appears in a log, an exception, or a URL. Every
//      diagnostic here names the KEY and the SCHEME, never the value.
//   3. A secret never enters a project file. This class only ever touches the
//      QSettings object it is given and the platform credential store --
//      neither is reachable from anywhere a .qxp is written or read.
//   4. "Remember" off means off: that policy is the CALLER's (an instance is
//      simply never asked to store() anything), so there is nothing here for
//      the caller to defeat by holding the value itself.
//   5. The `--test-case` default backend is `memory` via
//      SMARAGD_SECRET_BACKEND, read exactly like SMARAGD_AUDIO_BACKEND /
//      SMARAGD_MIDI_BACKEND -- so a headless suite run under `ctest -j` never
//      opens a real keychain (which can block on a UI prompt nobody can see)
//      and never writes into the developer's real smaragd.ini.
//
// Instantiation NEVER reaches for the SSettings singleton (see main/shell's
// CONTRACT.md and STATE.md for why that separation matters for `ctest -j`):
// the caller passes its own QSettings pointer in. That is what lets a unit
// test point this class at a private temp-file INI instead of the developer's
// real one -- the whole reason `secret_store_test` can run un-RUN_SERIAL
// alongside the qxa suite without ever touching a real credential.
class SSecretStore
{
public:
    enum class Result {
        Ok,             // *secret was filled in -- decrypted cleanly.
        Unset,          // nothing is stored under this key. Never conflated
                         // with an empty string: a caller that only checks
                         // the string could otherwise send "" as a password.
        Undecryptable   // something IS stored under this key but cannot be
                         // recovered: corrupt ciphertext, a foreign-machine
                         // blob, or a scheme tag this build does not compile.
                         // Never guessed at and never silently retried under
                         // a different backend.
    };

    // `settings` is where the non-secret half (and, for `dpapi`, the
    // ciphertext) is read and written -- e.g. "<key>/passwordScheme" and
    // "<key>/passwordEnc". It is NEVER touched for the `memory` or `none`
    // backends (AC 3a, AC 6), so it may be null for those two; every other
    // backend requires a real, non-null instance whose lifetime outlasts
    // this object. This object never takes ownership of it and never calls
    // anything on it except value()/setValue()/contains()/remove()/sync().
    //
    // `serviceName` scopes `keychain`/`libsecret` items (those stores are
    // keyed only by service+account, with nothing INI-like to give a test its
    // own file) so a caller -- in particular a unit test -- can use a name
    // distinct from the real "com.smaragd.media" service and never touch a
    // real credential (AC 6a).
    //
    // `backendOverride`, if non-empty, wins over SMARAGD_SECRET_BACKEND, which
    // wins over the platform default -- see resolveBackend().
    explicit SSecretStore( QSettings *settings,
                            QString serviceName = QStringLiteral( "com.smaragd.media" ),
                            const QString &backendOverride = QString() );
    ~SSecretStore();

    SSecretStore( const SSecretStore & ) = delete;
    SSecretStore &operator=( const SSecretStore & ) = delete;

    // "dpapi" | "keychain" | "libsecret" | "none" | "memory" -- whatever this
    // instance actually resolved to (resolveBackend()), never what was asked
    // for when the ask could not be honoured.
    QString backendName() const { return backend_; }

    // false only for "none": every other backend, including "memory", can
    // hold a secret past the call that stored it. A caller (the accounts
    // page, 5b) uses this to decide whether "Remember" is even offered.
    bool canPersist() const;

    // Encrypts/protects `secret` and persists it under `key` through this
    // instance's backend. `key` is an opaque namespace the caller owns (e.g.
    // "media/nextcloud/<accountId>") -- nothing here interprets it beyond
    // appending its own suffixes.
    //
    // Returns false, and persists NOTHING, when the backend cannot persist at
    // all ("none") or when the underlying platform call failed. Never a
    // silent partial write: on false, no key this call could have written is
    // left behind half-updated.
    bool store( const QString &key, const QString &secret );

    // Fills *secret on Result::Ok only; *secret is left UNTOUCHED on Unset or
    // Undecryptable, so a caller that forgets to check the Result cannot walk
    // away with a stale or default-constructed value mistaken for a real one.
    Result retrieve( const QString &key, QString *secret ) const;

    // Removes whatever is stored under `key`: the INI keys and, for
    // keychain/libsecret, the platform item too. A no-op, not an error, when
    // nothing was stored under `key`.
    void remove( const QString &key );

    // The backend this build/environment would resolve to, without needing
    // an instance -- e.g. so a caller can report it before opening a store.
    // Resolution order: `backendOverride` if non-empty, else
    // SMARAGD_SECRET_BACKEND, else the platform's compiled-in default, else
    // "none". `dpapi|keychain|libsecret|memory|none` are recognised
    // case-insensitively; "default" (or unset) means "ask the platform".
    // Asking for a real backend this build did not compile warns once and
    // falls back to the platform default -- it never returns a name this
    // build cannot actually serve.
    static QString resolveBackend( const QString &backendOverride = QString() );

private:
    QSettings *settings_;
    QString    service_;
    QString    backend_;

    QString schemeKey_( const QString &key ) const;
    QString encKey_( const QString &key ) const;
};

#endif
