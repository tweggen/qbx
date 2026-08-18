# Proposal 38: Media browser

> **Status: PROPOSED (2026-08-18).** A dockable media browser: pick a data
> source (local file system, Nextcloud), browse its tree or search it
> incrementally, filter by media type, and drag a file onto the timeline.
> Six gates, each one PR, each independently gateable and sized for a single
> subagent.
>
> **Scope for the MVP:** two sources (local, Nextcloud/WebDAV), one media
> category (audio), browse + incremental search + drag-to-timeline. **No**
> BPM/key analysis, **no** metadata extraction, **no** audition — see §F.

---

## A. What exists today, and what is missing

### A.1 The one panel that is nearly this, and why it is not

`SExternFileList` (`main/model/src/sexternfilelist.cpp`, 285 lines) is a
`QTreeWidget` in a dock (`dock_extern_file_list`) that lists **the files the
CURRENT PROJECT already references** plus its live assets, with a ref count per
row. It is a *project inventory*, not a browser: it cannot show a file the
project has never used, has no notion of a directory, no filter, no search and
no data source other than "whatever `SProject::externFiles()` holds".

Two things in it are directly reusable and this proposal reuses both rather
than re-inventing them:

- **The drag payload.** `startDrag()` (`:103`) sets one MIME type,
  `application/x-smaragd-resource`, whose payload is `file:<absolute path>` or
  `asset:<name>`, plus a hand-painted ghost pixmap.
- **The drop side already accepts it.** `SMVActualView::dropEvent`
  (`main/timeline/src/sstdmixerview.cpp:4144`) parses that payload, resolves
  the drop x/y to `(timePos, trackPath)` — index-path, so a lane nested in a
  folder works — and submits `SAddSampleAction` for `file:` or
  `SPlaceAssetAction` for `asset:`. It also normalises an **OS file drop**
  (`text/uri-list`) into the same `file:` payload, so there is exactly one
  placement path.

**Consequence, and it shapes the whole plan: a browser that emits
`file:<abspath>` for a local file needs ZERO changes to the timeline.** Gate 2
therefore ships drag-to-timeline without touching a single line of the 4000-line
arranger. Only remote files (gate 3) need a new payload and a new branch.

### A.2 Getting a sample into a project today

Three routes, all of them a file dialog or a file manager:

| Route | Site |
|---|---|
| Insert sample… | `SStdMixerView::ctInsertSample` — `QFileDialog::getOpenFileName` with the filter `*.wav *.mp3 *.flac *.aiff *.aif *.ogg *.opus` (`sstdmixerview.cpp:458`) |
| Drag from the OS file manager | `dropEvent`'s `hasUrls()` branch (`:4151`) |
| Drag from the resources dock | only for a file the project ALREADY references |

There is no way to keep a sample library open beside the arranger, and no way
at all to reach a file that is not on this machine's file system.

### A.3 There is no network code in the tree

`grep -rn "Qt6::Network\|QNetworkAccessManager" smaragd/` returns **nothing**,
and `find_package` names `Core Widgets Xml` only (`smaragd/CMakeLists.txt:52`).
Qt6::Network ships in qtbase, so this costs no new external dependency and no
vcpkg work — but it is the first HTTP client in this codebase and every
convention for one is being set here. Qt Xml is already linked, so a WebDAV
`PROPFIND` response parses with `QDomDocument` and needs nothing new either.

### A.4 What the user asked for

a) pick a data source (local FS, Nextcloud, later OneDrive …); b) browse a file
tree where one is available; c) incrementally search, recursive or not, under
the selected path; d) a media-type dropdown checkbox list; and drag the result
onto the timeline. Docked, floating or closed, exactly as the resources dock is.

---

## B. Design

### B.1 Two new modules, and why the split is a LAYER boundary

```
main/media/          — app_core layer.  No widget, ever.
main/mediabrowser/   — app_ui  layer.  The dock. Rank of pluginui / eventui.
```

`main/media` holds the source ABI, the two providers, the local cache and the
drop helper. `main/mediabrowser` holds the dock and nothing else. The split is
a **compile-time** boundary, not a convention: `main/CMakeLists.txt` builds
`app_core` as an OBJECT library that publishes only the lower layers' include
dirs, so a provider that includes a widget header **fails to compile**. That is
worth a module boundary on its own, and it also buys three things:

- the testkit (app_ui) can gate the provider layer directly, with no dock;
- `app/timeline`'s drop handler can reach `SMediaCache` (a `timeline → media`
  edge in `check_layering.py`) without reaching a UI slice it must not see;
- a later importer, a command-line tool or a batch fetch has a provider layer
  to call that does not start an event loop over widgets.

`Qt6::Network` is linked on `app_core` and therefore propagates upward. That is
the one impurity and it is accepted: layers, not modules, are the CMake targets.

**CMake and checker edits, exhaustively** (these are the mechanical parts a
subagent must not have to discover):

| File | Edit |
|---|---|
| `smaragd/CMakeLists.txt:52-53` | add `Network` to both `find_package` component lists |
| `smaragd/main/CMakeLists.txt` | `APP_CORE_FILES` += `media/…`; `app_core` include dir += `media/include`; `target_link_libraries(app_core … Qt::Network)` |
| `smaragd/main/CMakeLists.txt` | `APP_UI_FILES` += `mediabrowser/…`; `app_ui` include dir += `mediabrowser/include` |
| `tools/check_layering.py` | `APP_DEPS['media'] = {'model'}`; `APP_DEPS['mediabrowser'] = {'actions','media','model','shell'}`; `APP_DEPS['timeline'] += 'media'`; `APP_DEPS['shell'] += 'mediabrowser'`; `APP_DEPS['testkit'] += {'media','mediabrowser'}`; `APP_ENG['media'] = _ENG_BASE`; `APP_ENG['mediabrowser'] = _ENG_BASE` |

`media` depends on `model` only (for `SFilePathRef` and nothing else) and on NO
engine module beyond the `core`/`graph` base every module gets. It must never
grow an edge to `objects/*` — a provider that knows what an `SCut` is has
started doing the dock's job.

### B.2 The source ABI

```cpp
// app/media/smediaref.h — one file, one place, addressable
struct SMediaRef {
    QString sourceId;    // "local" | "nextcloud:<accountId>"
    QString path;        // source-relative, '/'-separated, never percent-encoded
    QString toUri() const;              // "smedia://local/C:/samples/kick.wav"
    static SMediaRef fromUri( const QString & );
};

struct SMediaEntry {
    QString  name;        // display name, decoded
    SMediaRef ref;
    bool     isDir = false;
    qint64   sizeBytes = -1;   // -1 unknown
    QDateTime modified;        // invalid = unknown
    QString  etag;             // remote only; "" locally
};

class SMediaSource : public QObject {
    Q_OBJECT
public:
    enum Cap { CanBrowse = 1, CanSearch = 2, CanStream = 4, NeedsFetch = 8 };
    virtual QString id() const = 0;
    virtual QString displayName() const = 0;
    virtual int     caps() const = 0;

    // Every call is ASYNC and returns a request id. Results arrive on the
    // MAIN thread, tagged with that id.
    virtual int  listDirectory( const QString &dirPath ) = 0;
    virtual int  search( const QString &rootPath, const QString &needle,
                         bool recursive, const QStringList &suffixes ) = 0;
    virtual int  fetch( const QString &filePath, const QString &destPath ) = 0;
    virtual void cancel( int requestId ) = 0;
signals:
    void entriesReady( int requestId, const QVector<SMediaEntry> &batch,
                       bool final, int truncatedCount );
    void requestFailed( int requestId, const QString &message );
    void fetchProgress( int requestId, qint64 done, qint64 total );
    void fetchFinished( int requestId, const QString &localPath );
};
```

Six invariants, and they are the whole reason the ABI looks like this:

1. **Everything is async and id-tagged.** A recursive walk of a sample library
   is seconds of work; a `PROPFIND` is a network round trip. Neither may run on
   the GUI thread. `SLocalMediaSource` walks on a `QThreadPool` (a **Qt-owned**
   thread — `QMetaObject::invokeMethod(..., Qt::QueuedConnection)` back to the
   source is the only way results cross); `SWebDavMediaSource` uses
   `QNetworkAccessManager`, which is async by construction. **No `std::thread`
   anywhere in this module**: proposal 19's rule that a non-Qt thread must
   never emit a Qt signal (Qt adopts the thread and the teardown join
   deadlocks) applies here exactly as it does in the engine.
2. **Supersession is by ID, never by a flag.** The view keeps the id of the
   request it is displaying and **drops any batch whose id is not that one**.
   A late result from a cancelled walk therefore cannot repaint over a newer
   one — by construction, not by a race the cancel is supposed to win. Same
   shape as proposal 21's "superseded by replacing the handle".
3. **Results STREAM.** `entriesReady` is called with batches (every ~200
   entries or ~100 ms, whichever first) with `final=false`, then once with
   `final=true`. That is what makes the search *incremental* in the sense the
   user asked for: a recursive walk shows its first hits immediately.
4. **A truncation is ANNOUNCED, never silent.** `truncatedCount > 0` on the
   final batch means the walk hit its bound (`kMaxSearchEntries = 5000`,
   `kMaxSearchDepth = 12`, `kMaxInFlightPropfinds = 4`) and the view must say
   so in a footer line. The repo's own rule: "if a workflow bounds coverage,
   log what was dropped — silent truncation reads as *covered everything*".
5. **A provider never touches the model and never submits an action.** It
   returns entries and local paths. Placement is the dock's and the drop
   handler's business.
6. **`cancel()` is advisory and results after it must be safely droppable.**
   The walker checks an atomic flag between entries; a reply already in flight
   is `abort()`ed and its finish handler must tolerate being called anyway.

### B.3 The media-type filter

A `QToolButton` with a checkable `QMenu` (the "dropdown checkbox list"), one
entry per category. The category → suffix mapping lives in **one** place:

```cpp
// app/media/smediatypes.h
namespace smedia {
    enum Category { Audio = 1, Midi = 2 };          // Midi declared, not shipped
    const QStringList &suffixesFor( int categoryMask );
    inline const QStringList kAudio = { "wav","aiff","aif","flac","ogg","oga",
                                        "opus","mp3" };
}
```

**The list is what the IMPORTER can actually decode, and that is a decision,
not an oversight.** Import runs through `twSampleSource` → libsndfile (+mpg123
for MP3); the existing Insert-sample filter is `*.wav *.mp3 *.flac *.aiff *.aif
*.ogg *.opus` and this matches it exactly. The request named **mp4** — *AAC in
an MP4 container is not decodable by the current import path*, so listing it
would offer the user a file that fails at the drop. It is deliberately absent;
adding AAC is an import-path change (a libsndfile build with the relevant
feature, or a decoder), not a browser change, and it is named in §F.

`Midi` exists in the enum and in the menu's construction so the control is
multi-category by shape — the user asked for that — but the MVP registers only
`Audio`, and the menu therefore shows one always-checked entry. A `.mid` drop
already works through `SProject::linkToFile`'s suffix dispatch
(`sproject.cpp:487`), so shipping the Midi category later is a one-line change
plus a case.

### B.4 The dock

The **seventh** `QDockWidget`, created in the `SMainWindow` ctor beside the
existing six, following `dock_clip_properties` verbatim:

```cpp
qDockMediaBrowser_ = new QDockWidget( tr( "Media Browser" ), this );
qDockMediaBrowser_->setObjectName( "dock_media_browser" );   // load-bearing
mediaBrowser_ = new SMediaBrowserPanel( qDockMediaBrowser_ );
qDockMediaBrowser_->setWidget( mediaBrowser_ );
addDockWidget( Qt::LeftDockWidgetArea, qDockMediaBrowser_ );
qDockMediaBrowser_->hide();        // first run only; restoreState() overrides
```

**Docked / floating / closed needs no new code and no settings key.** The
`objectName` is what `QMainWindow::saveState()`/`restoreState()` key on, and
that blob is already persisted as `ui/windowState` (written in `closeEvent`,
read by `restoreWindowLayout()`). Two consequences carried over from proposal
31: the dock is created in the **ctor**, because shell CONTRACT inv. 4 fixes the
restore order (`openMostRecent()` → `restoreWindowLayout()` → `show()`) and
`restoreState` can only place docks that already exist; and a `ui/windowState`
written by an older build has no entry for `dock_media_browser`, so on first
upgrade it keeps the ctor defaults (left area, hidden) — correct behaviour, and
the reason it starts hidden rather than stealing space from the arranger.

A `toggleViewAction()` goes in the View menu beside the other six. No shortcut
in the MVP.

Layout, top to bottom:

```
[ Source ▾ ] [ Media type ▾ ]           <- source combo, filter checkbox menu
[ ← ] [ path/breadcrumb..............]  <- current path, editable
[ search…................] [x] recurse  <- debounced 250 ms, supersedes
+------------------------------------+
| tree / result list (Name, Size, Modified)
+------------------------------------+
| footer: "2314 items, showing 5000 (truncated)" | busy spinner
```

Two modes over one view: **browse** (a `QTreeWidget` populated lazily on expand
via `listDirectory`) and **search** (a flat result list). An empty search box
returns to browse mode. Sizing follows `SExternFileList`'s hard-won policy —
`QSizePolicy::Preferred/Expanding`, `minimumWidth 200`, `maximumWidth 360` — for
exactly the reason its comment gives: a `QTreeWidget` is Expanding by default
and `QMainWindow` will otherwise hand it all the resize space and pin the
arranger at its minimum.

Per-user state in `SSettings`, under new `SOpt` keys: `media/lastSourceId`,
`media/lastPath/<sourceId>`, `media/categoryMask`, `media/searchRecursive`.

### B.5 Drag out — two payloads, one placement path

| Row is | Payload | Timeline change |
|---|---|---|
| A **local** file | `file:<absolute path>` | **none** — the existing branch handles it |
| A **remote** file | `media:<uri>` | one new branch (gate 2) |

The ghost pixmap is lifted from `SExternFileList::startDrag`. A directory row
is not draggable. A multi-selection drags its first file only, matching the
drop handler's existing "one clip per drop" comment.

The `media:` branch in `SMVActualView::dropEvent`, in full:

```cpp
} else if( payload.startsWith( QStringLiteral( "media:" ) ) ) {
    const SMediaRef ref = SMediaRef::fromUri( payload.mid( 6 ) );
    smediadrop::placeWhenLocal( ref, trackPath, timePos, this );
}
```

`smediadrop::placeWhenLocal` (in `main/media`) is the ONLY new asynchrony in
the placement path, and it does exactly one thing: get a local path, then
submit the **existing** `SAddSampleAction`. It never invents a clip, never
touches `SCut`, and never adds an action type.

**PLACEMENT IS ALWAYS `add-sample` OVER A LOCAL PATH. The media layer's job
ends at producing that path.** Everything below follows from that sentence.

### B.6 The cache, and where a fetched file actually ends up

Two stages, and they are different things:

1. **The cache** is where a fetch always lands:
   `<configDir>/mediacache/<sourceId-hash>/<content-key>.<ext>`, where the
   content key is `sha1(sourceId + path + etag|mtime + size)`. It is
   content-addressed so a re-fetch of an unchanged file is free, and
   LRU-evicted against a cap (`SOpt::MediaCacheCapMB`, default 2048) with the
   eviction **logged**.
2. **The project copy.** On a drop, if the project has a file path, the cached
   file is copied to `<projectdir>/media/<name>` and the clip references THAT.
   If the project is unsaved, the clip references the cache path directly and a
   status-bar message plus a `TW_LOG` warning say so.

**The second stage exists because of `SFilePathRef`.** A `.qxp` stores sample
paths portably — relative to the project file, else relative to `~`, else
absolute — and the app-config cache directory is machine-local, so a clip
pointing into it serialises as an **absolute path that means nothing on the
next machine**. Copying into the project directory makes the stored spelling
relative, which is what the whole of `main/model/sfilepathref.h` exists to
achieve. Relocating an already-placed cache reference at Save time is a real
follow-up and is **not** in the MVP (§F); the resources dock's existing
*Cleanup…* dialog is the neighbouring precedent for that kind of housekeeping.

**`SMARAGD_MEDIA_CACHE_DIR=<path>` relocates the cache; `=off` disables it**
(a fetch then goes to a per-request temp file and is not reused). This mirrors
`SMARAGD_SIDECAR_DIR` and exists for the same reason: the cache is a shared
per-user directory and `ctest -j4` runs four processes against it. The sidecar
store's hard-won lesson applies verbatim — **the temp file must be per-writer**
(`<path>.<pid>.<seq>.tmp`, or just `QSaveFile`, which does this correctly) or
two concurrent fetches of one key truncate and interleave into one file and one
of them publishes the mixture.

### B.7 The Nextcloud connector

Nextcloud speaks WebDAV. Everything the MVP needs is three verbs over HTTPS:

| Need | Request |
|---|---|
| List a directory | `PROPFIND <base>/<path>` with `Depth: 1` and a body requesting `d:displayname d:getcontentlength d:getlastmodified d:getcontenttype d:resourcetype d:getetag` |
| Search | client-side BFS of `Depth: 1` PROPFINDs, ≤ 4 in flight, bounded and announced (§B.2 inv. 4) |
| Download | `GET <base>/<path>` streamed to a `QSaveFile` in the cache |

Base URL: `https://<host>/remote.php/dav/files/<username>/`. Auth: **HTTP Basic
with a Nextcloud app password** over TLS — the standard, revocable credential,
and the one to tell the user to create.

Search is deliberately client-side. WebDAV `SEARCH`/DASL is available on
Nextcloud but its support varies by version and configuration, and a
client-side BFS is (a) identical in shape to the local walk, so one set of
bounds, one set of batching rules and one set of tests covers both, and (b)
cannot fail on a server that answers `SEARCH` with a 501. Server-side search is
a follow-up, behind the `CanSearch` capability bit that already exists for it.

Four rules that are not negotiable:

- **TLS errors are surfaced, never ignored.** No `ignoreSslErrors()` anywhere.
  A self-signed server reports the error text into `requestFailed`.
- **A 401 does not retry.** It disables the account for the session and says
  so; a retry loop against a rate-limited server is how an account gets locked.
- **Every reply is parented to its source** and `abort()`ed in the source's
  destructor. A `QNetworkReply` outliving the dock is a use-after-free with a
  network-shaped delay on it.
- **The PROPFIND response is parsed off the GUI thread** (`QtConcurrent::run` +
  a queued result). A directory of 10 000 entries is a megabyte of XML.

### B.8 Credentials

Qt ships no keychain and this proposal does not add QtKeychain. Accounts live
in `SSettings`:

```
media/nextcloud/<accountId>/url
media/nextcloud/<accountId>/user
media/nextcloud/<accountId>/password      <- only when "Remember" is checked
```

The password is stored **in plain text in `smaragd.ini`**, and the accounts
dialog says exactly that next to the checkbox. With "Remember" off, the
password is held in memory for the session and the key is not written. This is
honest rather than good; the mitigation that makes it acceptable is the app
password — a per-application, revocable credential that is not the user's
account password. A keychain integration is a follow-up (§F).

### B.9 The twelve traps, decided up front

| # | Trap | Decision |
|---|---|---|
| T1 | A recursive search on the GUI thread freezes the app | Every walk is off-thread and batched; the log dock's rule applies — bound a UI tick by **time**, never by a count |
| T2 | A superseded search result repaints over a newer one | Dropped by **request id**, not by a cancel flag winning a race (§B.2 inv. 2) |
| T3 | A `QNetworkReply` outliving its source | Parented to the source, aborted in its dtor; every handler is `QPointer`-guarded |
| T4 | A non-Qt thread emitting a Qt signal | `QThreadPool`/`QtConcurrent` only. No `std::thread` in either module |
| T5 | MinGW `thread_local` with a non-trivial destructor corrupts the heap at 3+ threads | POD thread-locals only in the walker (a known, paid-for bug in this repo) |
| T6 | A headless case writing `smaragd.ini` races another under `ctest -j` | Any qxa case that writes a `media/…` key is **`RUN_SERIAL`**, restores its key, and declares in its header that it OWNS it — the `midi_options_page` precedent |
| T7 | Two concurrent processes tearing one cache file | `QSaveFile` (per-writer temp + atomic rename) and `SMARAGD_MEDIA_CACHE_DIR` for isolation — the sidecar store's exact lesson |
| T8 | Breaking the existing drag payload | Local rows emit `file:` verbatim; the `file:` and `asset:` branches are not touched |
| T9 | Offering a file the importer cannot decode | The filter's suffix list IS the importer's; mp4/AAC is excluded and named (§B.3) |
| T10 | A dock built for a `describe()` gate is never shown, so lazy paint-time state is empty | Build it off screen and push it its state explicitly — the `assert-plugin-strip` / `grabHead` pattern |
| T11 | A cache path in a saved `.qxp` is machine-local | Copy into `<projectdir>/media/` when the project has a path; warn loudly when it does not (§B.6) |
| T12 | A silent bound (search truncation, cache eviction, in-flight cap) reads as complete coverage | Each one logs and the footer shows it (§B.2 inv. 4) |

---

## C. The gates

Six gates, one PR each, in order — each builds on the last and each is green on
its own. **Every gate ends with the standing gate list**: `./build.sh`,
`python tools/check_layering.py`, `python tools/check_logging.py`,
`ctest --test-dir smaragd/build -j4 --output-on-failure`, and a reconciled
count (**174 + N registered / 171 + N run / 3 disabled** on a non-Apple box,
where N is the cases that gate adds). That is not repeated per gate below.

Each gate's brief is self-contained: the files, the wiring, the ACs, and an
explicit *do not touch* list.

---

### GATE 1 — the source ABI and the local provider (no UI, no network)

**Deliverable.** `main/media/` exists, builds in `app_core`, and can list and
search a local directory tree asynchronously.

**Files.**

```
main/media/include/app/media/smediaref.h        SMediaRef, SMediaEntry
main/media/include/app/media/smediatypes.h      categories -> suffixes
main/media/include/app/media/smediasource.h     the abstract ABI
main/media/include/app/media/smediaregistry.h   id -> source, singleton
main/media/include/app/media/slocalmediasource.h
main/media/src/*.cpp
main/media/CONTRACT.md
main/media/tests/media_source_test.cpp          ctest target
tests/media/…                                   committed fixture tree
```

The fixture tree is small, committed, and shaped to make every AC a closed
form — e.g. `tests/media/lib/{kick.wav, snare.wav, notes.txt}` and
`tests/media/lib/sub/{hat.wav, deep/tom.wav}`, so "recursive finds 4 wavs,
non-recursive finds 2" is a fact about the repository rather than about the
machine. Reuse the existing `test_sawtooth.wav` bytes for the audio ones so no
new audio content is added.

**Also.** `smaragd/CMakeLists.txt` gains `Network` in both `find_package` lines
(gate 3 needs it; adding it here keeps gate 3 from touching the root file), and
`tools/check_layering.py` gains the `media` entries from §B.1.

**ACs.**

1. `media_source_test` is registered with CTest and green.
2. `listDirectory` on the fixture root returns directories before files, each
   sorted by name, with `isDir`, `sizeBytes` and `modified` populated for every
   entry; a non-existent path emits `requestFailed`, never an empty success.
3. The suffix filter applies: listing with the Audio mask omits `notes.txt`.
4. `search(root, "a", recursive=true)` finds every matching audio file under
   the tree; the same with `recursive=false` finds only those directly in
   `root`. Both figures are exact against the committed fixture.
5. A search emitting more than one batch delivers `final=true` exactly once and
   the union of the batches equals the full result set.
6. **Supersession:** issuing a second search while the first is running and
   dropping the first's id yields no entry tagged with the stale id after the
   test's own cancel point.
7. **Cancel:** a cancelled recursive search stops emitting within a bounded
   number of further batches, and `cancel()` on an unknown id is a no-op.
8. **Threading:** every `entriesReady` / `requestFailed` / `fetchFinished`
   emission is asserted to arrive on the main thread
   (`QThread::currentThread() == qApp->thread()`).
9. `grep -rn "std::thread" main/media/` is empty.
10. `main/media/CONTRACT.md` exists and states inv. 1-6 of §B.2 verbatim.

**Do not touch.** Any widget. `main/timeline`. The drag or drop paths. Any
existing test.

---

### GATE 2 — the dock: browse, filter, incremental search, drag out (local)

**Deliverable.** The Media Browser dock is on screen, browses the local file
system, searches it incrementally, filters by media type, and a dragged local
file lands on the timeline as a clip.

**Files.**

```
main/mediabrowser/include/app/mediabrowser/smediabrowserpanel.h
main/mediabrowser/src/smediabrowserpanel.cpp
main/mediabrowser/CONTRACT.md
main/shell/src/smainwindow.cpp        + the 7th dock, + the View menu entry
main/shell/include/app/shell/smainwindow.h   + describeMediaBrowser() test hook
main/servicesui/include/app/servicesui/soptions.h  + 4 SOpt keys (§B.4)
main/testkit/src/smediatestactions.cpp   the verbs below
docs/ACTIONS.md                          one row per new verb
tests/cases/media_browser_browse.qxa
tests/cases/media_browser_search.qxa
tests/cases/media_browser_drag_local.qxa
```

**New verbs** (registered exactly like `assert-meter`, §`SActionRegistry::
instance().registerType`):

| Verb | Attributes |
|---|---|
| `media-browser-source` | `sourceId` |
| `media-browser-path` | `path` |
| `media-browser-search` | `needle`, `recursive` = "0", `waitMs` = "2000" (waits for `final`, so a case never sleeps blindly) |
| `media-browser-filter` | `categories` = "audio" (comma-separated) |
| `media-browser-drag` | `row` \| `name`, `trackPath`, `timePos` — builds the REAL `QMimeData` the panel's `startDrag` builds and hands it to the REAL `SMVActualView::dropEvent` |
| `assert-media-browser` | `contains`, `absent`, `rowCount` = "-1", `truncated` = "-1", `mode` = "" (browse\|search) — matches `SMediaBrowserPanel::describe()` |

`describe()` is a single line in the house format:
`mode=search|source=local|path=…|filter=audio|rows=4|truncated=0|busy=0` followed
by one `name,size,dir` triple per row. `media-browser-drag` goes out through
the shell (`SMainWindow::mediaBrowserDrag`), because testkit may not include
`app/timeline` — the same route `drag-clip-edge` and `assert-lane-alignment`
already take.

**The panel is built OFF SCREEN and never shown under `--test-case`**, and the
verbs push it its state explicitly (T10). The `media-browser-drag` verb is the
only route to the drag payload, following testkit inv. 5's rule: a verb that
writes the model directly would pass while the gesture is broken.

**ACs.**

1. The dock exists with `objectName == "dock_media_browser"`, appears in the
   View menu via `toggleViewAction()`, and is hidden on a first run.
2. **Layout state round-trips.** Float it, close it, restart: the state is
   restored from `ui/windowState` with no new settings key. Verified manually
   and recorded in the PR body — the Qt state blob is not scriptable.
3. `media_browser_browse.qxa`: selecting the local source and setting the path
   to the fixture tree yields the exact row set (`rowCount`, `contains` for
   each name, `absent` for `notes.txt` under the Audio filter). Expanding a
   directory row lists its children.
4. `media_browser_search.qxa`: a recursive search for a fixture substring
   returns the exact count; the same non-recursive returns its exact count;
   **three searches issued back to back leave the panel showing the THIRD one's
   results** (the supersession AC, at the UI level this time).
5. Clearing the search box returns `mode=browse` at the same path.
6. `media_browser_drag_local.qxa`: `media-browser-drag` onto `trackPath="0"` at
   `timePos="48000"` places exactly one clip whose link start is 48000
   (`assert-clip-window`), and **one undo removes it**. A render of the result
   has the fixture's RMS (`assert-audio-energy`), proving the file the browser
   named is the file that sounds.
7. A drag from a **directory** row is refused and places nothing.
8. The panel's suffix filter and the Insert-sample dialog's filter come from
   the same `smedia::kAudio` — asserted by a grep in the PR body, since the
   dialog string is built from it.
9. `media/*` settings keys are written only by the panel; the three cases above
   do not write any (they set state through verbs, not settings), so **none of
   them needs `RUN_SERIAL`** — state that explicitly in the PR body.

**Do not touch.** `SMVActualView::dropEvent` — the `file:` branch already
handles this gate entirely (§A.1). `main/media`'s ABI.

---

### GATE 3 — the cache, the `media:` payload, and deferred placement

**Deliverable.** A file that is not yet on this machine can be dropped: the
drop starts the fetch and the clip appears when it lands, as one undo step.

**Files.**

```
main/media/include/app/media/smediacache.h     content-addressed, LRU, env knob
main/media/include/app/media/smediadrop.h      placeWhenLocal()
main/media/src/smediacache.cpp, smediadrop.cpp
main/media/tests/media_cache_test.cpp
main/timeline/src/sstdmixerview.cpp            the `media:` branch (5 lines)
main/servicesui/…/soptions.h                   + SOpt::MediaCacheCapMB
tests/cases/media_drop_deferred.qxa
```

Gate 3 has no network, so it needs a source that is remote-shaped:
**`SDelayedLocalSource`**, a test-only provider registered under
`sourceId="testdelay"` **only when `SMARAGD_MEDIA_TEST_SOURCE=1`**. It reads
the same fixture tree, reports `NeedsFetch`, and copies with a configurable
delay and a configurable failure. This is the same instinct as `twtestclap` —
an in-repo fixture that makes the path gateable without installing anything —
and it is what lets the deferred branch be tested deterministically instead of
against a real server's timing.

**ACs.**

1. `media_cache_test` green: a content key is stable across calls and changes
   when etag/mtime/size changes; a second `ensureLocal` of a cached key does
   **not** re-fetch (asserted by a fetch counter); eviction past the cap
   removes least-recently-used entries and **logs** what it removed;
   `SMARAGD_MEDIA_CACHE_DIR=off` still yields a usable local path and reuses
   nothing.
2. A ready ref places **synchronously** and is exactly one undo step.
3. A pending ref places **exactly once** when the fetch lands, at the frame and
   track the drop named, and is exactly one undo step. Asserted with
   `SDelayedLocalSource` at a delay long enough that the drop returns first.
4. A **failed** fetch places nothing, logs an error
   (`assert-log level="error"`) and shows a status message.
5. Closing the project while a placement is pending cancels it: nothing is
   placed, and no crash (the pending list is `QPointer`-guarded and cleared on
   project close).
6. A second drop of the same pending ref while the first is in flight results
   in **two** clips, not one and not three — the pending list is keyed by
   placement, not by ref.
7. **Portability (T11):** with a saved project, a dropped remote file is copied
   to `<projectdir>/media/` and the saved `.qxp` stores a **relative** path
   (assert with `assert-file-contains` over the saved XML). With an unsaved
   project, the clip references the cache and a warning is logged.
8. Two processes fetching the same key concurrently leave one intact file
   (`QSaveFile`; assert by hash) — run as a scripted two-process check like
   `qxa.instrument_render_determinism_xproc`.
9. The `file:` and `asset:` branches of `dropEvent` are byte-unchanged
   (`git diff` in the PR body shows only the added `else if`).

**Do not touch.** Any action class. `SAddSampleAction` is used as-is; no new
placement verb is created.

---

### GATE 4 — the Nextcloud/WebDAV connector, gated against a stub server

**Deliverable.** `SWebDavMediaSource` browses, searches and downloads from a
Nextcloud server, gated end to end with no network and no account.

**Files.**

```
main/media/include/app/media/swebdavclient.h      PROPFIND / GET, one QNAM
main/media/include/app/media/swebdavmediasource.h
main/media/src/swebdavclient.cpp, swebdavmediasource.cpp
main/media/tests/webdav_stub.{h,cpp}      QTcpServer speaking PROPFIND+GET
main/media/tests/webdav_source_test.cpp
tests/cases/media_webdav_browse.qxa
tests/cases/media_webdav_drop.qxa
```

**The stub is the gate.** A `QTcpServer` on `127.0.0.1:0` that answers
`PROPFIND` with a canned multistatus body built from a table and `GET` with the
bytes of a fixture WAV, and that can be told to answer `401`, `404`, `500`, to
stall, or to close mid-body. Plain HTTP, no TLS — TLS is Qt's code, not ours,
and a self-signed certificate in the repo would be a liability. That leaves
"TLS error surfacing" as manual (§C.6).

**ACs.**

1. `webdav_source_test` green against the stub.
2. **PROPFIND parsing**, each its own assertion: display names; sizes;
   `getlastmodified` in RFC 1123 parsed to a correct `QDateTime`; directories
   distinguished by `<d:resourcetype><d:collection/>`; the request's own
   collection excluded from its listing; **percent-encoded hrefs decoded**
   (`Drum%20Kit/kick%231.wav` → `Drum Kit/kick#1.wav`); trailing slashes
   normalised; an entry with a missing optional property yields `-1`/invalid
   rather than being dropped.
3. **Search:** a recursive search issues depth-1 PROPFINDs breadth-first with
   **at most 4 in flight** (asserted by the stub counting concurrent requests),
   streams batches, and stops at `kMaxSearchEntries` with `truncatedCount > 0`
   and a log line naming the bound. Non-recursive issues exactly one PROPFIND.
4. **Errors:** `401` → one `requestFailed` carrying the status, **no retry**
   (the stub counts requests); `404` → `requestFailed`, no crash; a connection
   closed mid-body → `requestFailed` and **no partial file left in the cache**.
5. **Cancel:** a cancelled GET leaves no file and no `.tmp` behind.
6. `fetchProgress` is emitted at least twice for a body large enough to arrive
   in several chunks, and monotonically.
7. Destroying the source with requests in flight aborts them and does not
   crash (run under the test's own teardown; note that no sanitizer build
   exists in this repo, so this is an assertion of behaviour, not of memory).
8. `media_webdav_browse.qxa` and `media_webdav_drop.qxa` drive the REAL panel
   against a stub started by the case, and the drop's rendered audio matches
   the fixture's RMS — the full chain, from PROPFIND to a sounding clip.
9. `ignoreSslErrors` appears nowhere: `grep -rn "ignoreSslErrors" smaragd/` is
   empty.

**Do not touch.** The panel's UI code (the source is chosen by id; the panel is
already source-agnostic from gate 2). The cache.

---

### GATE 5 — accounts, credentials, and the Options page

**Deliverable.** A user can add a Nextcloud account in Edit → Options → Media,
test it, and see it in the browser's source picker.

**Files.**

```
main/servicesui/src/soptionsdialog.cpp     a new "Media" page
main/servicesui/include/app/servicesui/soptionsdialog.h  + describeMediaPage()
main/shell/include/app/shell/ssettings.h   account accessors (§B.8)
main/testkit/src/smediatestactions.cpp     + assert-media-options
tests/cases/media_options_page.qxa         RUN_SERIAL, owns its keys
docs/ACTIONS.md                            + the verb row
```

The page mirrors the MIDI page's build/load/apply triple (servicesui CONTRACT
inv. 7) — a list of accounts, Add/Edit/Remove, and a per-account form of URL,
username, app password, a **Remember password** checkbox with the plain-text
warning beside it, and a **Test connection** button reporting the HTTP status.
`assert-media-options` builds the REAL `SOptionsDialog` off screen and matches
`describeMediaPage()`, exactly as `assert-midi-options` does.

**ACs.**

1. An account round-trips through `SSettings`: add, restart, still listed, and
   the browser's source combo offers `nextcloud:<accountId>`.
2. With **Remember off**, `media/nextcloud/<id>/password` is **absent from the
   INI** (asserted by reading the file), and the source is usable for the rest
   of the session.
3. With Remember on, the key is present, and the dialog shows the plain-text
   warning (`assert-media-options contains="plaintext"`).
4. Removing an account removes every one of its keys and closes any open
   source for it.
5. **Test connection** against the gate-4 stub reports success; against a
   `401` it reports the status text and does not save a broken account
   silently.
6. `media_options_page.qxa` is **`RUN_SERIAL`**, declares in its header that it
   OWNS the `media/nextcloud/qxatest/*` keys, and restores them, leaving
   `smaragd.ini` byte-identical (T6, the `midi_options_page` precedent —
   verify with an md5 across a full `-j4` run and say so in the PR body).
7. An account whose URL is not parseable is refused at the dialog, not at the
   first request.

**Do not touch.** The provider layer. The cache.

---

### GATE 6 — closeout: contracts, docs, and the manual runbook

**Deliverable.** The feature is documented where this repo documents things,
and the parts no headless gate can reach have a runbook.

**Files.**

```
main/media/CONTRACT.md          completed (all invariants, all knobs)
main/mediabrowser/CONTRACT.md   completed
main/timeline/CONTRACT.md       + the `media:` drop branch invariant
main/shell/CONTRACT.md          + the 7th dock
docs/ARCHITECTURE.md            + both modules in the map
docs/MEDIA_BROWSER_MANUAL_GATE.md   the runbook (new)
CLAUDE.md                       a "Media browser (proposal 38)" section
plan/STATE.md                   the execution record
plan/proposed/38_MEDIA_BROWSER.md   status → EXECUTED, findings appended
```

`docs/MEDIA_BROWSER_MANUAL_GATE.md` follows `docs/ASIO_WINDOWS_GATE.md`: a
numbered, reproducible procedure against **a real Nextcloud server**, with a
place to record the result. It covers exactly what §D says is ungated — TLS
against a real certificate, a real app password, a large directory (≥ 2000
entries), a large file (≥ 100 MB) with progress and cancel, a slow/flaky link,
and a server with a self-signed certificate.

**ACs.**

1. Every invariant asserted in §B appears in a `CONTRACT.md` next to the code
   that must uphold it.
2. `docs/ACTIONS.md` has a row for all seven new verbs with their real
   attribute defaults, and `action_roundtrip_test` covers each (it enumerates
   the registry, so a verb missing `writeXml`/`readXml` symmetry fails).
3. The CLAUDE.md section states, in the house's own idiom, the three things a
   newcomer will otherwise get wrong: placement is always `add-sample` over a
   local path (§B.5); a cache path in a saved project is not portable, hence
   the project copy (§B.6/T11); and results are dropped by request id, not by
   winning a cancel race (§B.2 inv. 2).
4. The manual runbook has been **run once** and its result recorded in the
   file, as `docs/ASIO_WINDOWS_GATE.md` requires of itself.
5. `plan/STATE.md` carries the chronological record, including anything the
   execution found that this design got wrong.

---

## D. What is gated, and what is NOT

Stated here so no PR body has to imply coverage that does not exist.

**Gated (headless, every platform):** the source ABI's async contract,
supersession and cancel; the local walk and its filter; the panel's browse and
search modes and their row sets; the drag payload through the real drop
handler; placement, undo and project-relative persistence; the cache's keying,
reuse, eviction and cross-process safety; the WebDAV client's parsing, error
handling, concurrency cap, truncation, progress and cancel — against a stub;
the accounts page's load/apply and its credential rule.

**NOT gated, and named in every PR body that touches it:**

- **A real Nextcloud server.** TLS against a real certificate, real app-password
  auth, a real server's PROPFIND dialect, redirects, and rate limiting. Manual
  runbook only (§C.6).
- **Network physics** — throughput, latency, a flaky link, a resumed transfer.
  A resumed transfer is not implemented at all (§F).
- **Dock geometry.** Docked/floating/closed round-trips through Qt's opaque
  `ui/windowState` blob; no verb can inspect it. Manual, once, per gate 2 AC 2.
- **Pixels.** No `paintEvent` of the panel is gated. `screenshot` grabs the
  screen's root window, blank under `QT_QPA_PLATFORM=offscreen`, so it proves
  nothing about one widget — the same limitation `SRecordingRendererInline`
  already carries.
- **Drag ergonomics.** The ghost pixmap, the hover feedback, the auto-scroll of
  the arranger during a drag: synthesised drops go straight to `dropEvent`.
- **A tree of pathological size** (100 k files, a symlink loop, a network mount
  that hangs on `stat`). The bounds exist and are logged; they are not measured.
- **Very large downloads.** The stub serves a fixture WAV; a 100 MB body's
  memory behaviour is manual.

---

## E. Risks

1. **Qt6::Network is the first HTTP client in this tree.** Nothing else depends
   on it, so the blast radius is contained to `main/media`, but it also means
   there is no house precedent for reply lifetime, proxies or TLS. §B.7's four
   rules and gate 4's ACs are the precedent being set. *Mitigation:* the stub
   server makes the client's behaviour observable without a server.
2. **A plain-text password in `smaragd.ini`.** Accepted, labelled, and mitigated
   by app passwords (§B.8). A keychain is a follow-up. If the requester judges
   this unacceptable, the fallback is prompt-per-session, which is gate 5 minus
   the persistence branch — a smaller change, not a larger one.
3. **The cache in a saved project.** T11's copy-into-the-project rule covers the
   saved case; the unsaved case is a warning, and relocation-on-save is not in
   the MVP. A user who drops a remote file into an unsaved project, saves, and
   moves the project to another machine gets a missing sample — which the app
   already handles as a missing-file placeholder, not a crash.
4. **`ctest -j` and the shared cache.** T6 and T7 are the two ways this repo has
   already been bitten (the `smaragd.ini` ownership convention and the sidecar
   temp-name collision). Both mitigations are copied verbatim rather than
   reinvented.
5. **Scope creep toward a full asset manager.** Favourites, tags, ratings,
   collections and audition are all reasonable and all excluded. §F is the
   list; adding to it is a decision, not a slip.
6. **Without audition, the browser is a file picker with a tree.** This is the
   most likely thing to be asked for the moment it ships. It is excluded
   deliberately (it needs a preview player, a lane that is not the arranger's,
   and a decision about what happens to the transport), and it is the **first**
   follow-up.

---

## F. Non-goals, and the follow-ups they become

Explicitly **not** in this proposal, each with the reason:

| Not doing | Why, and what it would take |
|---|---|
| **Audition / preview playback** | Needs a preview player outside the arranger's transport and a decision about what a preview does to a running playback. The first follow-up. |
| **BPM / key / metadata analysis** | Requester deferred it explicitly. The analysis sidecar machinery (proposal 27) is the natural home; the browser would consume it, not compute it. |
| **AAC / MP4 (`.m4a`)** | The import path (libsndfile + mpg123) cannot decode it, so listing it would offer a file that fails at the drop (§B.3). An import-path change, not a browser change. |
| **MIDI in the filter** | The category exists in the enum and the drop path already handles `.mid`; only registration and a case are missing. Shipped when asked for. |
| **OneDrive / Google Drive / S3** | The ABI is shaped for them (§B.2) — each is one `SMediaSource` subclass plus an OAuth flow, and OAuth is the real work, not the listing. |
| **Server-side WebDAV `SEARCH`** | Behind the `CanSearch` capability bit. Client-side BFS ships first because it cannot fail on a server that answers 501 (§B.7). |
| **Resumable / range downloads** | A cancelled fetch restarts. Range requests need a partial-file protocol in the cache. |
| **Writing back to a source** | Upload, rename, delete. The browser is read-only, and saying so is a feature. |
| **Favourites, tags, ratings, collections** | An asset manager, not a browser. |
| **Waveform thumbnails in the browser** | Needs a preview probe per row; the preview machinery is per-`SCut`, not per-file-on-a-server. |
| **Relocating an already-placed cache reference at Save time** | §B.6/T11. The resources dock's *Cleanup…* dialog is where it would belong. |
| **A keychain for credentials** | QtKeychain is a new external dependency; the app-password mitigation makes it deferrable (§B.8). |

---

## G. Summary of the six gates

| Gate | Ships | Gated by |
|---|---|---|
| 1 | `main/media`: the ABI + the local provider, async, bounded, id-tagged | `media_source_test` over a committed fixture tree |
| 2 | The dock: source picker, tree, filter, incremental search, drag out (local) | 3 qxa cases through the REAL panel and the REAL drop handler |
| 3 | The cache, the `media:` payload, deferred placement, project-relative copy | `media_cache_test` + a deferred-drop case + a two-process cache race |
| 4 | The Nextcloud/WebDAV connector | `webdav_source_test` + 2 qxa cases, against an in-repo stub server |
| 5 | Accounts, credentials, the Options → Media page | `media_options_page.qxa` (RUN_SERIAL) + `assert-media-options` |
| 6 | Contracts, docs, CLAUDE.md, STATE.md, the manual runbook | the runbook, run once and recorded |
