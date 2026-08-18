// app/shell/smediaaccountmanager.h -- the Nextcloud accounts model
// (proposal 38 §C GATE 5b).
//
// Owns everything an account needs that main/media may not know exists:
// persistence of url/user (SSettings) and the password (SSecretStore, §B.8),
// the smedia::CredentialProvider implementation (installed once at
// startup), and the live SWebDavMediaSource instances registered with
// SMediaRegistry so the browser's source combo offers "nextcloud:<id>"
// (design AC 7). One instance, owned by SApplication for the process
// lifetime -- the same shape as SMidiOutPump / SLiveMonitor: a dialog reads
// a persistent object rather than keeping its own state, so an account
// added through Options survives the dialog closing and is immediately
// live for the dock.
//
// See main/shell/CONTRACT.md for the invariants this class exists to
// uphold (§B.8 rules 1-5, T13/T14/T17 in proposal 38 §B.9).

#ifndef SMEDIAACCOUNTMANAGER_H
#define SMEDIAACCOUNTMANAGER_H

#include "app/media/smediacredentials.h"

#include <QHash>
#include <QObject>
#include <QSettings>
#include <QString>
#include <QStringList>

#include <memory>

class SSecretStore;
class SWebDavMediaSource;

// password=set|unset|undecryptable -- describeMediaPage() (AC 15) and every
// account-listing verb print exactly this word, NEVER the secret, its
// length, or a prefix.
enum class SMediaPasswordStatus { Unset, Set, Undecryptable };

struct SMediaAccountInfo
{
    QString              accountId;
    QString              url;
    QString              user;
    SMediaPasswordStatus passwordStatus = SMediaPasswordStatus::Unset;
};

class SMediaAccountManager : public QObject, public smedia::CredentialProvider
{
    Q_OBJECT
public:
    explicit SMediaAccountManager( QObject *parent = nullptr );
    ~SMediaAccountManager() override;

    SMediaAccountManager( const SMediaAccountManager & )            = delete;
    SMediaAccountManager &operator=( const SMediaAccountManager & ) = delete;

    QStringList       accountIds() const;   // sorted
    SMediaAccountInfo account( const QString &accountId ) const;

    // Validates `url` and REFUSES here -- never at the first request (AC 13)
    // -- before touching anything: on refusal nothing is persisted and
    // nothing is registered. On success, url/user are always persisted; the
    // password is persisted through SSecretStore only when `remember` is
    // true AND the resolved backend canPersist() (AC 8/9/10) -- otherwise
    // NO passwordScheme/passwordEnc key is written, ever, and any
    // previously-remembered secret for this account is scrubbed (Remember
    // going from on to off means off). Either way a live SWebDavMediaSource
    // is (re)registered with SMediaRegistry using the header this call
    // computes, so a session-only password still browses THIS session
    // (AC 9's "usable for the rest of the session").
    bool setAccount( const QString &accountId, const QString &url,
                     const QString &user, const QString &password,
                     bool remember, QString *error = nullptr );

    // Removes every key (url, user, passwordScheme, passwordEnc) AND the
    // stored secret itself (a keychain/libsecret item left behind outlives
    // the app -- AC 11), unregisters and deletes the live source.
    void removeAccount( const QString &accountId );

    // "dpapi" | "keychain" | "libsecret" | "none" | "memory" -- whatever
    // SSecretStore actually resolved to on this run.
    QString secretBackendName() const;
    bool    canRememberPasswords() const;
    // A human sentence for the page: names the backend, or -- for `none` --
    // states plainly that nothing is persisted and why Remember is off
    // (§B.8: "no weak tier, so no tier whose strength anyone has to reason
    // about").
    QString backendDescription() const;

    // Synchronous (a short, bounded local QEventLoop) PROPFIND against `url`
    // with HTTP Basic for user/password -- the "Test connection" button
    // (AC 12). Touches no stored account. Returns a short status report,
    // e.g. "200 OK (3 entries)", "401 Unauthorized", "invalid URL", or
    // "timed out" -- NEVER the password, and never the Authorization header.
    QString testConnection( const QString &url, const QString &user,
                            const QString &password, int timeoutMs = 5000 );

    // smedia::CredentialProvider. Empty when no usable credential is held
    // for `sourceId` ("nextcloud:<accountId>") -- an unknown source, an
    // un-remembered session-only password this process instance never
    // received, or an `undecryptable` secret.
    QString authorizationHeaderFor( const QString &sourceId ) const override;

    // Re-scans every persisted account for a legacy PLAINTEXT
    // `media/nextcloud/<id>/password` key (§B.8 "Migration": only a
    // pre-release dev build could have written one) and migrates it into
    // SSecretStore, removing the plaintext key either way -- never left
    // behind beside the ciphertext (AC 17). Run once at construction; public
    // because the real trigger this is written for -- an app LAUNCH that
    // finds one already on disk -- cannot be reproduced inside one running
    // process, so the options page re-runs it every time it loads (a cheap,
    // harmless re-check) and the qxa case relies on exactly that.
    void rescanForPlaintextMigration();

private:
    static QString secretKey( const QString &accountId );
    QString        basicHeader( const QString &user, const QString &password ) const;
    void           registerSourceWithPassword( const QString &accountId, const QString &user,
                                               const QString &url, const QString &password );
    void           unregisterSource( const QString &accountId );

    // A private QSettings pointed at the SAME on-disk INI SSettings::instance()
    // uses (same IniFormat/UserScope/"Smaragd"/"smaragd" quadruple -- Qt
    // shares one QConfFile per path within a process, so the two coexist
    // safely). SSecretStore deliberately never reaches for SSettings itself
    // (its header explains why: its OWN test must never touch the real INI),
    // so the composition root that owns a REAL, persistent SSecretStore has
    // to hand it a QSettings explicitly -- this is that QSettings.
    mutable QSettings             secretSettings_;
    std::unique_ptr<SSecretStore> secretStore_;

    // accountId -> the live source, owned via Qt parent-child (parented to
    // `this`); SMediaRegistry does NOT own its sources (main/media/CONTRACT).
    QHash<QString, SWebDavMediaSource *> sources_;
};

#endif // SMEDIAACCOUNTMANAGER_H
