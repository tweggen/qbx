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
