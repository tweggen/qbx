#ifndef SMEDIATESTACTIONS_H
#define SMEDIATESTACTIONS_H

#include "app/actions/saction.h"

#include "tw/core/twtypes.h"

#include <QString>

// Headless coverage for proposal 38 gate 2 — the Media Browser dock.
//
// Every one of these drives the REAL SMediaBrowserPanel through the shell
// (testkit CONTRACT inv. 5: testkit may not include app/timeline, and the drag
// has to reach both the panel and the arranger). The panel is BUILT OFF SCREEN
// and never shown under `--test-case` (design trap T10), so these verbs push it
// its state explicitly and `SMediaBrowserPanel::describe()` is the oracle.
//
// NOTHING HERE SLEEPS. The provider ABI is async by construction, so every verb
// that issues a request waits for the panel to go IDLE — no live root request,
// no pending lazy expand, no search still inside its 250 ms debounce — up to
// `waitMs`. A case that slept for a fixed time would flake under `ctest -j4`,
// which is exactly the load these bounds meet.

// media-browser-source — pick a source by id and OPEN it (§B.4: a source is
// contacted when it is SELECTED, never at startup).
//   sourceId = "local"
//   waitMs   = "2000"
class SMediaBrowserSourceAction : public SAction {
public:
    QString name() const override
    { return QStringLiteral( "media-browser-source" ); }
    QStringList knownAttributes() const override
    { return { "sourceId", "waitMs" }; }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString sourceId_ = QStringLiteral( "local" );
    int     waitMs_   = 2000;
};

// media-browser-path — set the browse root, and optionally EXPAND one directory
// row through the real itemExpanded path (the lazy listDirectory a user's click
// on the triangle issues).
//   path   = ""      (empty = leave the path alone and only expand)
//   expand = ""      the NAME of a directory row to expand
//   waitMs = "2000"
class SMediaBrowserPathAction : public SAction {
public:
    QString name() const override
    { return QStringLiteral( "media-browser-path" ); }
    QStringList knownAttributes() const override
    { return { "path", "expand", "waitMs" }; }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString path_;
    QString expand_;
    int     waitMs_ = 2000;
};

// media-browser-search — enter search mode, or leave it with an empty needle.
//   needle    = ""
//   recursive = "0"
//   waitMs    = "2000"   0 = issue and return AT ONCE, which is how a case
//                        stacks three searches to gate supersession
//   debounce  = "0"      1 = let the REAL 250 ms timer fire (needs waitMs > 250)
class SMediaBrowserSearchAction : public SAction {
public:
    QString name() const override
    { return QStringLiteral( "media-browser-search" ); }
    QStringList knownAttributes() const override
    { return { "needle", "recursive", "waitMs", "debounce" }; }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString needle_;
    bool    recursive_ = false;
    int     waitMs_    = 2000;
    bool    debounce_  = false;
};

// media-browser-filter — the media-type checkbox menu.
//   categories = "audio"   comma-separated: "audio", "midi", "audio,midi", ""
//   waitMs     = "2000"
class SMediaBrowserFilterAction : public SAction {
public:
    QString name() const override
    { return QStringLiteral( "media-browser-filter" ); }
    QStringList knownAttributes() const override
    { return { "categories", "waitMs" }; }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString categories_ = QStringLiteral( "audio" );
    int     waitMs_     = 2000;
};

// media-browser-drag — build the REAL QMimeData the panel's startDrag() builds
// and hand it to the REAL SMVActualView::dropEvent at the pixel that names
// (trackPath, timePos). A DIRECTORY row is refused, so a case asserting that
// pairs this with expectReject="true".
//   row       = "-1"   depth-first row index, matching describe()
//   name      = ""     ...or the first row with this name (wins over `row`)
//   trackPath = "0"
//   timePos   = "0"
class SMediaBrowserDragAction : public SAction {
public:
    QString name() const override
    { return QStringLiteral( "media-browser-drag" ); }
    QStringList knownAttributes() const override
    { return { "row", "name", "trackPath", "timePos" }; }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString  name_;
    QString  trackPath_ = QStringLiteral( "0" );
    int      row_       = -1;
    qint64   timePos_   = 0;
};

// --------------------------------------------------------------------------
// proposal 38 GATE 5b -- the Nextcloud accounts model and Options -> Media.
// --------------------------------------------------------------------------

// set-media-account — calls SMediaAccountManager::setAccount() directly, the
// same call the dialog's Add/Save button makes (`set-option`'s convention:
// bypass the widget, exercise the SAME production code the widget calls).
//   accountId = "" (required)
//   url       = ""
//   user      = ""
//   password  = ""
//   remember  = "1"
//   expectOk  = "1"   fails the ACTION (not an assertion) on a mismatch, so
//                     AC 13's "refused, not saved" is a driving step, not a
//                     separate assert-* call
class SSetMediaAccountAction : public SAction {
public:
    QString name() const override
    { return QStringLiteral( "set-media-account" ); }
    QStringList knownAttributes() const override
    { return { "accountId", "url", "user", "password", "remember", "expectOk" }; }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString accountId_;
    QString url_;
    QString user_;
    QString password_;
    bool    remember_ = true;
    bool    expectOk_ = true;
};

// remove-media-account — SMediaAccountManager::removeAccount() (AC 11).
//   accountId = "" (required)
class SRemoveMediaAccountAction : public SAction {
public:
    QString name() const override
    { return QStringLiteral( "remove-media-account" ); }
    QStringList knownAttributes() const override
    { return { "accountId" }; }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString accountId_;
};

// media-test-connection — SMediaAccountManager::testConnection() (AC 12).
// `useStub` starts a THROWAWAY in-process SWebDavStub (never a fixed port,
// same discipline as gate 4's own test) so the case needs no external
// server: "ok" answers every PROPFIND with a small canned listing, "401"
// answers with a 401 fault on every path. `url` is ignored whenever `useStub`
// is non-empty.
//   url            = ""
//   user           = ""
//   password       = ""
//   useStub        = ""     "" | "ok" | "401"
//   expectContains = ""     substring the result string must contain
//   expectAbsent   = ""     substring that must NOT appear
class SMediaTestConnectionAction : public SAction {
public:
    QString name() const override
    { return QStringLiteral( "media-test-connection" ); }
    QStringList knownAttributes() const override
    { return { "url", "user", "password", "useStub", "expectContains", "expectAbsent" }; }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString url_;
    QString user_;
    QString password_;
    QString useStub_;
    QString expectContains_;
    QString expectAbsent_;
};

// media-account-redaction-drive — proposal 38 AC 14/T13, ONE self-contained
// driving action (so the whole flow sits inside ONE `assert-log` window,
// SAssertLogAction::beginAction()'s rule): starts a throwaway in-process
// stub, saves a REAL account through SMediaAccountManager::setAccount()
// (exercising SSettings + SSecretStore for real, which is what "sets a
// password" needs to mean for the redaction gate to be honest), then issues
// a PROPFIND that SUCCEEDS ("a browse") and one that gets a 401 fault
// ("a failing request") through the REAL registered SWebDavMediaSource.
// Never asserts anything itself — a following `assert-log` does that.
//   accountId = "qxatest"
//   user      = "qxauser"
//   password  = "" (required — this is the literal AC 14 checks for)
//   remember  = "1"
class SMediaAccountRedactionDriveAction : public SAction {
public:
    QString name() const override
    { return QStringLiteral( "media-account-redaction-drive" ); }
    QStringList knownAttributes() const override
    { return { "accountId", "user", "password", "remember" }; }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString accountId_ = QStringLiteral( "qxatest" );
    QString user_      = QStringLiteral( "qxauser" );
    QString password_;
    bool    remember_  = true;
};

// assert-media-options — the Options -> Media page, built off screen, same
// house pattern as assert-midi-options: match against describeMediaPage().
//   contains / absent   substrings of the describe() dump
//   accountCount        exact count (-1 = not checked)
class SAssertMediaOptionsAction : public SAction {
public:
    QString name() const override
    { return QStringLiteral( "assert-media-options" ); }
    QStringList knownAttributes() const override
    { return { "contains", "absent", "accountCount" }; }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString contains_;
    QString absent_;
    int     accountCount_ = -1;
};

// assert-settings-file — the ON-DISK smaragd.ini, resolved via
// SSettings::instance().configDir() rather than a path the script would have
// to guess at (there is no per-platform smaragd.ini path expressible in a
// .qxa). AC 8/9's "grep the INI for the plaintext password: absent" and "no
// passwordScheme/passwordEnc key was written" both read this file directly,
// the same way `assert-file-contains` reads a saved .qxp.
//
// A plain whole-file substring check, DELIBERATELY not scoped to an INI
// "section": QSettings::IniFormat only brackets the FIRST path component as
// a "[group]" header ("media/nextcloud/qxatest/url" is written under
// "[media]" as the single key "nextcloud\qxatest\url=..."), so a
// scoping scheme built around "[section]" headers would silently never
// match anything below the top level and pass every case vacuously. Callers
// scope for themselves instead, by writing the full disk-spelled key
// (`nextcloud\qxatest\passwordScheme`) — the account id already makes it
// unique, which is the only scoping an account-keyed assertion needs.
//   contains = ""   substring that must be present
//   absent   = ""   substring that must NOT be present
class SAssertSettingsFileAction : public SAction {
public:
    QString name() const override
    { return QStringLiteral( "assert-settings-file" ); }
    QStringList knownAttributes() const override
    { return { "contains", "absent" }; }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString contains_;
    QString absent_;
};

// assert-media-browser — match SMediaBrowserPanel::describe().
//   contains  = ""     substring that must appear
//   absent    = ""     substring that must NOT appear
//   rowCount  = "-1"   exact row count (-1 = not checked)
//   truncated = "-1"   exact truncation count (-1 = not checked)
//   mode      = ""     "browse" | "search"
//   waitMs    = "0"    wait for the panel to go idle first
class SAssertMediaBrowserAction : public SAction {
public:
    QString name() const override
    { return QStringLiteral( "assert-media-browser" ); }
    QStringList knownAttributes() const override
    { return { "contains", "absent", "rowCount", "truncated", "mode",
               "waitMs" }; }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString contains_;
    QString absent_;
    QString mode_;
    int     rowCount_  = -1;
    int     truncated_ = -1;
    int     waitMs_    = 0;
};

#endif  // SMEDIATESTACTIONS_H
