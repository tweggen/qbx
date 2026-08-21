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
    { return { "path", "expand", "activate", "waitMs" }; }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString path_;
    QString expand_;
    // activate= DOUBLE-CLICKS the named row, through the panel's real
    // itemDoubleClicked slot: a directory NAVIGATES into it (path field,
    // persisted last-path and listing all move together), a file is a no-op.
    // Sequenced after path=/expand= for the same reason expand= is sequenced
    // after path=: the row has to exist first.
    QString activate_;
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
//   row            = "-1"   depth-first row index, matching describe()
//   name           = ""     ...or the first row with this name (wins over `row`)
//   trackPath      = "0"
//   timePos        = "0"
//   belowLastTrack = "0"    target the EMPTY canvas space below the last lane
//                           instead of a track (trackPath is then ignored) —
//                           the drop handler's own new-track gesture.
class SMediaBrowserDragAction : public SAction {
public:
    QString name() const override
    { return QStringLiteral( "media-browser-drag" ); }
    QStringList knownAttributes() const override
    { return { "row", "name", "trackPath", "timePos", "belowLastTrack" }; }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString  name_;
    QString  trackPath_      = QStringLiteral( "0" );
    int      row_            = -1;
    qint64   timePos_        = 0;
    bool     belowLastTrack_ = false;
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
    { return { "url", "user", "password", "useStub", "accountId", "useAccountUrl",
               "expectContains", "expectAbsent" }; }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString url_;
    QString user_;
    QString password_;
    QString useStub_;
    // accountId= drives SMediaAccountManager::testAccountConnection() -- what
    // the BUTTON calls -- instead of the bare testConnection(). With
    // useAccountUrl="1" the url and user come from the saved account too, so
    // the verb holds exactly what the dialog's form holds after an account is
    // selected or saved: url and user filled in, PASSWORD EMPTY. That empty
    // box is the whole point; it is the state the button is pressed in.
    QString accountId_;
    bool    useAccountUrl_ = false;
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
//   initialPage         the SOptionsDialog ctor argument (-2 = "omit it",
//                        the default; -1 or unset means "use whatever the
//                        no-arg ctor does", which is AC-b1's remembered
//                        page). Passing a real index (e.g. 2 = Audio) is
//                        what makes "an explicit page still wins over the
//                        remembered one" assertable — `describeMediaPage()`
//                        starts with `page=<name>` for exactly this.
class SAssertMediaOptionsAction : public SAction {
public:
    QString name() const override
    { return QStringLiteral( "assert-media-options" ); }
    QStringList knownAttributes() const override
    { return { "contains", "absent", "accountCount", "initialPage" }; }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString contains_;
    QString absent_;
    int     accountCount_ = -1;
    // -2 means "the attribute was not given" -- construct the dialog with
    // NO explicit page (the ordinary `SOptionsDialog dialog(nullptr)` every
    // other case exercises), so a case that never mentions initialPage sees
    // no change in behaviour from before AC-b1.
    int     initialPage_ = -2;
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

// --------------------------------------------------------------------------
// Proposal 38 GATE 3 — the deferred `media:` drop
// --------------------------------------------------------------------------

// media-test-source — configure SDelayedLocalSource, the test-only provider
// that is remote-SHAPED (NeedsFetch + a delayed copy). It exists only when
// SMARAGD_MEDIA_TEST_SOURCE=1, so this verb REFUSES when it is not registered
// rather than silently doing nothing.
//
//   delayMs  = "150"   how long a fetch takes. Long enough that the drop
//                      handler returns FIRST is what makes AC 3 a real
//                      deferred placement rather than a synchronous one.
//   failPath = ""      a substring; a fetch whose source path contains it FAILS
//                      instead of copying (AC 4). "" clears it.
//   reset    = "0"     1 = zero the source's fetch counter AND smediadrop's
//                      placed/failed/abandoned counters.
//   clearCache = "0"   1 = empty the media cache. REFUSED (and the verb fails)
//                      unless SMARAGD_MEDIA_CACHE_DIR named the root, so a case
//                      run without the knob cannot wipe a developer's real
//                      cache. A gate that asserts a FETCH COUNT on a cold key
//                      needs this: a cache directory surviving between runs
//                      would make the second run measure a hit.
class SMediaTestSourceAction : public SAction {
public:
    QString name() const override
    { return QStringLiteral( "media-test-source" ); }
    QStringList knownAttributes() const override
    { return { "delayMs", "failPath", "reset", "clearCache" }; }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    int     delayMs_ = -1;        // -1 = leave alone
    QString failPath_;
    bool    hasFailPath_ = false;
    bool    reset_ = false;
    bool    clearCache_ = false;
};

// media-drop-wait — pump the event loop until no `media:` placement is still
// pending, then optionally assert what happened.
//
// NOTHING HERE SLEEPS FOR A FIXED TIME, for the same reason nothing else in
// this file does: the fetch finishes when it finishes, and a case that slept
// long enough on an idle box would flake under `ctest -j4`.
//
//   waitMs    = "8000"
//   placed    = "-1"   exact smediadrop::placedCount() (-1 = not checked)
//   failed    = "-1"   exact smediadrop::failedCount()
//   abandoned = "-1"   exact smediadrop::abandonedCount() — the T18 / project
//                      -changed path, i.e. "a fetch landed and DELIBERATELY
//                      placed nothing", which is a different claim from "no
//                      clip appeared"
//   fetches   = "-1"   exact SDelayedLocalSource::fetchCount(), which is how
//                      the cache-reuse assertion is made from a script
class SMediaDropWaitAction : public SAction {
public:
    QString name() const override
    { return QStringLiteral( "media-drop-wait" ); }
    QStringList knownAttributes() const override
    { return { "waitMs", "placed", "failed", "abandoned", "fetches" }; }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    int waitMs_    = 8000;
    int placed_    = -1;
    int failed_    = -1;
    int abandoned_ = -1;
    int fetches_   = -1;
};

// media-webdav-stub — start (or stop) the PROCESS-OWNED in-process WebDAV
// server the end-to-end cases browse and download from (proposal 38 §C GATE 5,
// sub-PR 5c, AC 18/19).
//
// HOW A CASE LEARNS THE PORT, AND WHY IT IS THIS WAY. The stub binds
// 127.0.0.1:0 — an OS-assigned port, never a fixed one, which is the whole
// reason four concurrent `ctest -j4` cases cannot collide over it. A .qxa is a
// static document: it cannot interpolate a value produced at run time into a
// later attribute, so a case can never write the URL down. Rather than invent a
// variable-substitution mechanism for one verb, THIS VERB WRITES THE URL WHERE
// THE REST OF THE FLOW READS IT: given an `accountId`, it calls
// SMediaAccountManager::setAccount() itself with the stub's own baseUrl(), so
// the account — and with it the "nextcloud:<accountId>" source in the browser's
// combo — exists the moment the verb returns. The case then names the SOURCE
// ID, which is static, and never the URL, which is not.
//
// The alternatives were considered and are worse: a fixed port re-introduces
// exactly the collision the OS-assigned one removes, and reporting the port
// into a log line the case greps would let a case READ the port and still not
// be able to USE it.
//
// ONE STUB PER PROCESS, parented to qApp so it cannot outlive the application
// object, and `action="stop"` tears it down explicitly at the end of a case
// (idempotent: stopping a stub that was never started is a no-op, which is what
// makes a defensive stop at the top of a case free).
//
//   action     = "start"   | "stop"
//   fixtureDir = ""        a LOCAL directory mirrored into the stub's PROPFIND
//                          table, recursively, REAL BYTES AND ALL — so a GET of
//                          lib/kick.wav returns the committed fixture's WAV and
//                          a dropped clip actually sounds. Relative to the
//                          runner's CWD (tests/cases), like every other path in
//                          a .qxa. Empty = leave the table as it is.
//   accountId  = ""        empty = do not touch the accounts model at all
//   user       = "qxauser"
//   password   = "qxapass"
//   remember   = "0"       0 = nothing is persisted for the secret, which is
//                          what keeps these cases out of the shared INI beyond
//                          the url/user pair they remove at the end
//   fault      = ""        "" | "401" | "404" | "500" | "closemidbody" |
//                          "stall" — EMPTY CLEARS EVERY INJECTED FAULT, so a
//                          case restores the healthy server with one line
//   faultPath  = ""        the request path the fault applies to ("" = the root
//                          listing)
//   expectAuth = ""        THE `Authorization` HEADER VALUE THE SERVER DEMANDS,
//                          in full ("Basic <base64 of user:password>"). Empty =
//                          the header is not inspected at all, which is the
//                          pre-gate-6 behaviour every other case relies on.
//                          When it is set, any request whose header does not
//                          match EXACTLY is answered 401 -- ahead of `fault=`,
//                          because a real server rejects a credential before it
//                          dispatches a method.
//
//                          IT IS NOT STICKY, exactly as `fault=` is not: each
//                          invocation states the server's whole state, so a
//                          later `<media-webdav-stub fault="500" …/>` that does
//                          not repeat it clears it. Spelling the base64 LITERAL
//                          in the case is the point -- deriving it here from
//                          user=/password= would be a second implementation of
//                          SMediaAccountManager::basicHeader() checking itself,
//                          whereas a literal is what makes a green browse a
//                          statement about the BYTES on the wire.
//
// THE STUB IS NOT A NEXTCLOUD SERVER. Plain HTTP with no TLS, no redirects, no
// rate limiting, no WWW-Authenticate challenge and one canonical PROPFIND
// dialect. Since gate 6 it DOES check the credential when `expectAuth=` says
// to -- an exact string compare against one header value, which is the whole of
// its "authentication" and nothing like a real server's. What it gates is OUR
// half of the conversation; a real server is the manual runbook's job (§C.6,
// docs/MEDIA_BROWSER_MANUAL_GATE.md).
class SMediaWebDavStubAction : public SAction {
public:
    QString name() const override
    { return QStringLiteral( "media-webdav-stub" ); }
    QStringList knownAttributes() const override
    { return { "action", "fixtureDir", "accountId", "user", "password",
               "remember", "fault", "faultPath", "expectAuth" }; }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString action_     = QStringLiteral( "start" );
    QString fixtureDir_;
    QString accountId_;
    QString user_       = QStringLiteral( "qxauser" );
    QString password_   = QStringLiteral( "qxapass" );
    bool    remember_   = false;
    QString fault_;
    QString faultPath_;
    QString expectAuth_;
};

#endif  // SMEDIATESTACTIONS_H
