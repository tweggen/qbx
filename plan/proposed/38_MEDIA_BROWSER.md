# Proposal 38: Media browser

> **Status: PROPOSED (2026-08-18).** A dockable media browser: pick a data
> source (local file system, Nextcloud), browse its tree or search it
> incrementally, filter by media type, and drag a file onto the timeline.
> Six gates, each independently gateable and sized for a single subagent (gate
> 5 is three PRs).
>
> **Revised 2026-08-18 after an adversarial review** that verified the claims
> about the existing tree and found six real defects. What changed: §B.1's
> "the compiler enforces it" was false; the wiring table was missing
> `Qt::Concurrent` and a `shell → media` edge; §B.2's cross-thread hand-back was
> a use-after-free; gate 4's qxa half could not have been built; `kAudio` did
> not match the dialog it claimed to match; and §B.8's `obfuscated` fallback
> needed a cipher this tree does not link — it is now `none`. Traps T16-T20 and
> §G are the rest of that pass.
>
> **Scope for the MVP:** two sources (local, Nextcloud/WebDAV), one media
> category (audio), browse + incremental search + drag-to-timeline, and
> credentials **encrypted at rest** by the platform's own store (§B.8). **No**
> BPM/key analysis, **no** metadata extraction, **no** audition — see §F.
> Token auth (Nextcloud Login Flow v2, then OAuth2 where a deployment needs it)
> is the named direction and §B.8a is what keeps the MVP from blocking it.

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
| Insert sample… | `SStdMixerView::ctInsertSample` — a `QFileDialog` whose first entry is "Audio and MIDI" and whose "Audio files" entry is `*.wav *.mp3 *.flac *.aiff *.aif *.ogg *.opus` (`sstdmixerview.cpp:458-462`) |
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

The credential store (§B.8) is the same story: `crypt32` on Windows (MinGW
ships it) and `Security.framework` on macOS (in the SDK) are platform SDK
links, not dependencies to acquire. Only Linux's libsecret is genuinely
optional, and its absence degrades to a named fallback rather than to plain
text.

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
drop helper. `main/mediabrowser` holds the dock and nothing else.

**What the layer boundary actually enforces — and what it does not.** An
earlier draft of this section claimed a provider that includes a widget header
"fails to compile". **That is false**, and a subagent relying on it would get no
error: `app_model` links `Qt::Widgets` **PUBLIC**
(`smaragd/main/CMakeLists.txt:407-417`) and `app_core` links `app_model` PUBLIC
(`:425`), so every Qt widget header is available at every layer — indeed
`SExternFileList` is a `QTreeWidget` living in the LOWEST layer. What the build
enforces is only the `app/<module>/…` include graph, through per-layer include
directories; the finer module edges are `tools/check_layering.py`'s business.

"No widget in a provider" is therefore a **contract plus a grep**, not a
compiler error: it is stated in `main/media/CONTRACT.md` and gated by
`grep -rnE "#include <Q(Widget|TreeWidget|Dialog|Menu|Painter|Pixmap)" main/media/`
being empty (gate 1 AC 11). The module split is still worth having — it buys
three things:

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
| `smaragd/CMakeLists.txt:52-53` | add **`Network`** AND **`Concurrent`** to both `find_package` component lists. Concurrent is a SEPARATE Qt component and is absent from the tree today (`grep -rn Concurrent smaragd/*/CMakeLists.txt` is empty); §B.2 and §B.7 both call for `QtConcurrent`, so forgetting it is a link error at the end of gate 1, not at the start |
| `smaragd/main/CMakeLists.txt` | `APP_CORE_FILES` += `media/…`; `app_core` include dir += `media/include`; `target_link_libraries(app_core … Qt::Network Qt::Concurrent)` |
| `smaragd/main/CMakeLists.txt` | `APP_UI_FILES` += `mediabrowser/…`; `app_ui` include dir += `mediabrowser/include` |
| `tools/check_layering.py` | `APP_DEPS['media'] = {'model'}`; `APP_DEPS['mediabrowser'] = {'actions','media','model','shell'}`; `APP_DEPS['timeline'] += 'media'`; `APP_DEPS['shell'] += {'media','mediabrowser'}`; `APP_DEPS['testkit'] += {'media','mediabrowser'}`; `APP_ENG['media'] = _ENG_BASE`; `APP_ENG['mediabrowser'] = _ENG_BASE` |

**`APP_DEPS` is NOT transitive** — the allowed set is literally
`APP_DEPS[mod] | {mod}` (`check_layering.py:283`). So `shell` needs **`media`**
in its own set, not merely `mediabrowser`: gate 5 has the shell implement
`smedia::CredentialProvider`, which is an `app/media/…` include from `app/shell`
and fails the checker without it.

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
   the GUI thread. `SLocalMediaSource` walks on a **private, bounded**
   `QThreadPool`; `SWebDavMediaSource` uses `QNetworkAccessManager`, which is
   async by construction. **No `std::thread` anywhere in this module**:
   proposal 19's rule that a non-Qt thread must never emit a Qt signal (Qt
   adopts the thread and the teardown join deadlocks) applies here exactly as
   it does in the engine.

   **THE CROSS-THREAD HAND-BACK IS A WORKER `QObject` WITH A SIGNAL, NEVER
   `QMetaObject::invokeMethod` ON A RAW POINTER.** An earlier draft specified
   the latter and it is a use-after-free: the pool thread dereferences the
   source to post the event, and a source can be deleted at any moment — gate 5
   AC 11 deletes one on account removal, and teardown deletes them all. A
   `QPointer` does not fix it either; `QPointer` is only safe to read on the
   thread that owns the object, so T3's guard covers handlers already ON the
   main thread and nothing else. The walk therefore lives in a `QObject` whose
   `batchReady` signal is connected to the source with `Qt::QueuedConnection`,
   because **Qt tears connections down under its own mutex when the receiver
   dies** — a queued emission to a destroyed receiver is dropped, not crashed.
   The worker is owned by the pool task and `deleteLater`s itself. Gate 1 AC 12
   destroys a source mid-walk and is the only thing that will catch a subagent
   improvising the raw-pointer version, because the safe and unsafe forms are
   indistinguishable on a machine that never loses the race.

   **A private pool, not `QThreadPool::globalInstance()`, and a named hang.**
   Two threads, dedicated to this module. `cancel()` is checked between
   entries, so a walker blocked inside a single `stat()` — a dead network mount,
   a spun-down drive — **never sees it and pins its thread until the OS returns**.
   That is an accepted limitation, stated because it cannot be designed away
   without an out-of-process walker; what the private pool buys is that it
   cannot starve `QtConcurrent` or anything else Qt runs on the global pool.
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
    inline const QStringList kAudio = { "wav","mp3","flac","aiff","aif","ogg",
                                        "opus" };   // == the dialog's list
}
```

**The list is what the IMPORTER can actually decode, and that is a decision,
not an oversight.** Import runs through `twSampleSource` → libsndfile (+mpg123
for MP3); `kAudio` is **byte-for-byte the seven suffixes** of the existing
Insert-sample dialog's "Audio files" entry (`sstdmixerview.cpp:460`). An
earlier draft added `oga` to it and still claimed the two matched — they did
not, and because gate 2 AC 8 builds the dialog's filter string FROM `kAudio`,
that one extra suffix would have silently changed a shipping dialog. **Adding a
suffix to this list is a user-visible change to Insert-sample and needs its own
decision and its own line in a PR body**, which is the whole point of there
being one list. (`.oga` is a legitimate candidate — libsndfile reads it — but
it is a separate change.) The request named **mp4** — *AAC in
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
**Writing those keys is what makes every gate-2 case `RUN_SERIAL`** — see gate 2
AC 9; a panel that persists is a panel that touches the shared INI.

Four states the design owes an answer to, because they are the first things a
user meets and the easiest things to leave undefined:

| State | Behaviour |
|---|---|
| **First run**, no `media/lastPath` | The local source opens at the last Insert-sample directory (`SSettings::lastDir("sample", …)`) — the one place the app already remembers where this user keeps audio — then `QStandardPaths::MusicLocation`, then home |
| **A source that is unreachable** | Nothing is contacted at startup. A source is opened when it is SELECTED, and a failure paints an inline banner in the panel naming the error, with **Retry**; an `undecryptable` credential (§B.8) paints **Re-enter password** and opens the accounts page. A dock restored with `media/lastSourceId` pointing at a dead server therefore costs a banner, never a hang at launch |
| **No project open, or a project switch** | The panel is independent of the project — browsing is always available. Only a DROP needs a project, and a pending placement is dropped on a project change (§B.5) |
| **Symlinks on the local walk** | **Not followed.** It bounds a loop without relying on `kMaxSearchDepth`, and it makes gate 1's exact-count ACs a fact about the fixture rather than about the walker's mood |

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
    smediadrop::placeWhenLocal( ref, trackId, timePos, this );
}
```

`smediadrop::placeWhenLocal` (in `main/media`) is the ONLY new asynchrony in
the placement path, and it does exactly one thing: get a local path, then
submit the **existing** `SAddSampleAction`. It never invents a clip, never
touches `SCut`, and never adds an action type.

**IT CAPTURES THE TRACK'S IDENTITY, NOT ITS INDEX-PATH.** An index-path is a
position in a tree that the user is free to edit while a 40 MB file downloads:
delete a track above the target, or reorder a folder, and a path captured at
drop time now names **a different track** — a clip landing silently on the
wrong lane, which is worse than not landing at all. The pending placement
therefore holds the target `SObject`'s id, re-resolves it to a path at
completion, and **refuses with a status message if the track is gone**. Same
rule for the project: a pending placement is dropped when the project it was
made against is closed OR switched, not merely closed (`SApplication`'s
current-project change is the signal; a close is one case of it).

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

   **EVICTION MAY NEVER DELETE A FILE THE OPEN PROJECT REFERENCES.** The cap is
   a cache policy and the project is not a cache; without a pin, the honest
   reading of "LRU past 2 GB" is *a clip that plays today and is silent next
   week*, announced only by a log line nobody reads. Before evicting, the store
   subtracts every path in `SProject::externFiles()` — which is exactly the set
   of files the project has a clip over, already maintained. A cache that
   cannot get under its cap because the project pins too much **logs that and
   stops**, rather than evicting anyway.
2. **The project copy.** On a drop, if the project has a file path, the cached
   file is copied to `<projectdir>/media/<name>` and the clip references THAT.
   If the project is unsaved, the clip references the cache path directly and a
   status-bar message plus a `TW_LOG` warning say so.

   Three rules the copy needs and an earlier draft did not state:

   - **The name is SANITISED for the local file system.** WebDAV permits `:`,
     `?`, `*`, `|`, a trailing dot and a trailing space in a file name; Windows
     permits none of them, and this repo is tested on Windows first. Illegal
     characters become `_`, and the result is capped so that
     `<projectdir>/media/<name>` stays inside `MAX_PATH` on a default Windows
     configuration. (The design decodes `Drum%20Kit/kick%231.wav` proudly in
     gate 4 AC 2 and would then have failed to write it.)
   - **A name COLLISION does not overwrite.** Two different remote files both
     called `kick.wav` must not become one file and two clips over it — the
     second silently replacing the first's audio is a data-loss bug with no
     symptom. On collision with a file whose content key differs, the copy is
     `kick (2).wav`.
   - **A REPEAT drop of the same remote file reuses the existing copy.** Same
     content key, same target: no second file, no `(2)`. A user dragging the
     same loop onto four tracks gets one file.

   And one consequence to state rather than discover: **undo of a deferred drop
   removes the clip and leaves the copied file behind.** The copy is not part of
   `SAddSampleAction`, so its undo cannot know about it. This is survivable and
   has a precedent — the resources dock's *Cleanup…* dialog exists for exactly
   this class of orphan — but it is a real asymmetry and belongs in the
   CONTRACT, not in a bug report six months from now.

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

### B.8 Credentials — `SSecretStore`, and what "encrypted" honestly means

**A secret is never written to `smaragd.ini` in plain text.** It goes through
`SSecretStore` (`main/shell`, beside `SSettings`), which stores and retrieves a
secret by key name and whose backend is chosen at build time by platform:

| Backend | Where the bytes live | How protected |
|---|---|---|
| **`dpapi`** (Windows) | ciphertext, base64, in the INI | `CryptProtectData` with `CRYPTPROTECT_UI_FORBIDDEN`, **user-scoped**: the key is derived from the Windows logon credential and never leaves LSA. Links `crypt32` — present in MinGW, no new dependency |
| **`keychain`** (macOS) | in the **login keychain**, not in the INI at all | Security.framework `SecItemAdd` / `SecItemCopyMatching`, `kSecClassGenericPassword`, service `com.smaragd.media`. Links `Security.framework` — in the SDK, no new dependency |
| **`libsecret`** (Linux, when found) | in the Secret Service | libsecret, keyed the same way. An **optional** dependency, `TW_HAVE_LIBSECRET` |
| **`none`** (fallback) | **nowhere** | No real store on this platform ⇒ **"Remember" is disabled** and the password is kept for the session only. See below — this replaces an earlier `obfuscated` design |

The INI keeps only the non-secret half plus a scheme tag:

```
media/nextcloud/<accountId>/url
media/nextcloud/<accountId>/user
media/nextcloud/<accountId>/passwordScheme   dpapi | keychain | libsecret
media/nextcloud/<accountId>/passwordEnc      <- dpapi only; base64 ciphertext
```

**There is no encrypt-it-ourselves fallback, and that is a deliberate reversal.**
An earlier draft specified an `obfuscated` backend: AES-128 under a key derived
from a build salt. Two things killed it. **It needs a cipher this tree does not
have** — no crypto library is linked anywhere, and Qt6 ships `QCryptographicHash`
but no public AES — so it was either an unacknowledged new dependency or
hand-rolled AES in a DAW, and hand-rolled crypto in a codebase with no security
surface is the worst of the three options. And **its own documentation would
have had to say it protects nothing**, which is a scheme that exists to make a
checkbox feel safe. A platform with no real store now says so and does not
pretend: the password is session-only, the Remember checkbox is disabled with a
tooltip naming the reason, and the user can install libsecret (or use an app
password and re-enter it) with full knowledge of the trade. No secret is stored
weakly anywhere, and no code path can be reached that writes a password we
cannot protect.

Five rules, each of which is an AC in gate 5:

1. **The scheme tag is load-bearing.** A DPAPI blob is user- and machine-bound
   by design, so an INI copied to another machine, another user account, or a
   roamed `%APPDATA%` **cannot** decrypt — and must fail *readably*: the account
   reports `undecryptable`, the UI says "re-enter the password for this
   account", and nothing is sent to the server. Without the tag the failure
   would be a garbage credential in an `Authorization` header and a 401 nobody
   can explain.
2. **A secret never appears in a log, a `describe()`, an exception message or a
   URL.** `describeMediaPage()` reports `password=set|unset|undecryptable` and
   never a length, never a prefix. The `Authorization` header is redacted in
   every diagnostic — as `Authorization: Basic <redacted>`, so a log still shows
   WHICH scheme was used. This is gateable and is gated, and **what the case
   asserts absent is the password AND its base64 spelling**: a Basic header
   leaks `base64(user:password)`, not the password, so a case that greps only
   for the literal would pass while the credential sat in the log in plain
   sight. It must NOT assert `"Basic "` absent — that forbids the correctly
   redacted line and every sentence containing the word, forcing redaction to
   erase the scheme name to satisfy its own gate.
3. **A secret never enters a `.qxp`.** An account is machine-local
   configuration, like a device id. Nothing in this proposal touches the
   project file.
4. **"Remember" off still means off.** The password is held in memory for the
   session and no key of any kind is written.
5. **The `--test-case` default backend is `memory`.** A headless suite must not
   write into the developer's real keychain or Secret Service — and on macOS a
   keychain access from a background test can block on a UI prompt nobody can
   see, which is exactly the `qoffscreen` failure mode again (a case burning its
   whole timeout at ~0 % CPU). `SMARAGD_SECRET_BACKEND=dpapi|keychain|libsecret
   |none|memory` overrides ahead of the platform choice, mirroring
   `SMARAGD_AUDIO_BACKEND` and `SMARAGD_MIDI_BACKEND` exactly. **`memory` is a
   real store with process lifetime** — the options-page cases need a backend
   that round-trips, and they must not be the ones writing DPAPI blobs into the
   developer's INI.

**What this does and does not buy, stated plainly**, because a credential store
that oversells itself is worse than one that does not:

- On Windows, macOS and Linux-with-libsecret it is **real** encryption at rest,
  keyed off the user's login. Another user on the same box cannot read it; a
  stolen `smaragd.ini`, a backup, a synced config directory or a support-bundle
  upload does not carry a usable password.
- Everywhere else **nothing is persisted at all**. There is no weak tier, so
  there is no tier whose strength anyone has to reason about. A real backend
  that fails at RUNTIME (a locked keychain, a Secret Service that is not
  running) degrades to the same place — session-only, with a warning naming the
  reason — and never silently to a weaker store.
- **Nothing here changes what goes over the wire.** HTTP Basic sends the
  password to the server on every request; TLS is what protects that, and the
  app password is what limits the blast radius. Encryption at rest and
  encryption in transit are different problems and only the first one is solved
  here.
- **The plaintext is a `QString` in RAM while an account is open.** Qt strings
  are implicitly shared and copy-on-write, so there is no honest way to zero
  them; claiming a scrub would be false. The secret is fetched from the store
  at open and released when the last source for the account closes — that
  bounds the window, and no more than that is claimed.

**Migration:** any `media/nextcloud/<id>/password` plaintext key found (only a
pre-release dev build could have written one) is read once, re-stored through
`SSecretStore`, and the plaintext key is **removed** — never left behind beside
the ciphertext.

### B.8a Token-based auth is the direction, and the next step is not OAuth2

The requester's intent — move off a stored password to a token flow — is right,
and the concrete next step is **Nextcloud Login Flow v2**, not OAuth2:

- **Login Flow v2** (`POST /index.php/login/v2`, then poll the returned
  endpoint) opens the user's browser, has them authenticate against their own
  server — including any SSO or 2FA it uses — and hands back a **server-issued
  app password**, revocable from the user's security page. The user never types
  their real password into Smaragd, and **no client registration is needed on
  the server**.
- **OAuth2** on Nextcloud requires an administrator to register a client id and
  secret per deployment, and a desktop app cannot keep a client secret. It is
  the right answer for a hosted service we control, and the wrong first step for
  software a user points at their own server.

Either way the credential that comes back is a bearer-ish string that lands in
the **same `SSecretStore`** — which is why the store is being built now, with a
backend seam and a scheme tag, rather than a single hard-coded encrypt call.
Login Flow v2 is a follow-up (§F), not part of the MVP; what the MVP owes it is
not painting itself into a corner, and that is discharged by (a) the store, and
(b) `SWebDavClient` taking an **`Authorization` header value** rather than a
username/password pair, so a `Bearer` token is a caller change and not a client
change.

### B.9 The fifteen traps, decided up front

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
| T13 | A secret reaching a log, a `describe()`, an error string or a support bundle | Redacted at the source; gated by a case that greps the log ring for the password it just set (§B.8 rule 2) |
| T14 | A DPAPI blob copied to another machine decrypting to garbage and being SENT | The scheme tag makes the failure explicit — `undecryptable`, re-enter, nothing on the wire (§B.8 rule 1) |
| T15 | A headless case writing into the developer's real keychain, or blocking on an invisible macOS keychain prompt | `SMARAGD_SECRET_BACKEND=memory` is the `--test-case` default, ahead of the platform choice (§B.8 rule 5) |
| T16 | A pool thread posting a result to a source that is being deleted | The hand-back is a worker `QObject`'s SIGNAL, never `invokeMethod` on a raw pointer — Qt drops a queued emission to a dead receiver under its own mutex (§B.2 inv. 1). Gate 1 AC 12 |
| T17 | Eviction deleting audio the open project plays | The project's `externFiles()` are PINNED; a cache that cannot reach its cap says so and stops (§B.6) |
| T18 | A slow fetch completing onto a stale index-path — a clip on the WRONG track | The pending placement holds the track's OBJECT ID and re-resolves at completion; a vanished track places nothing (§B.5) |
| T19 | A remote name that is legal on WebDAV and illegal on Windows (`:`, `?`, trailing dot), or two remote files with one name | Sanitise, and disambiguate a collision as `name (2).ext` — never overwrite (§B.6) |
| T20 | Believing the compiler enforces "no widget in a provider" | It does not: `app_model` links `Qt::Widgets` PUBLIC. Contract + grep, gate 1 AC 11 (§B.1) |

---

## C. The gates

Six gates in order — each builds on the last and each is green on its own. One
PR each, **except gate 5, which is prescribed as three** (5a/5b/5c). **Every gate ends with the standing gate list**: `./build.sh`,
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
7. **Cancel:** a cancelled recursive search emits **at most one** further batch
   (the one already assembled when the flag was seen), and `cancel()` on an
   unknown id is a no-op. "Bounded" without a number is not checkable.
8. **Threading:** every `entriesReady` / `requestFailed` / `fetchFinished`
   emission is asserted to arrive on the main thread
   (`QThread::currentThread() == qApp->thread()`).
9. `grep -rn "std::thread" main/media/` is empty.
10. `main/media/CONTRACT.md` exists and states inv. 1-6 of §B.2 verbatim.
11. **No widget in a provider:** `grep -rnE "#include <Q(Widget|TreeWidget|Dialog|Menu|Painter|Pixmap)" main/media/`
    is empty. This is a grep, not a compiler error — see §B.1; the compiler will
    not catch it, because `app_model` links `Qt::Widgets` PUBLIC.
12. **A source destroyed mid-walk does not crash and delivers nothing after.**
    Start a recursive search over the fixture tree, delete the source while
    batches are in flight, spin the event loop. This is the AC that separates
    the signal/slot hand-back §B.2 inv. 1 requires from the raw-pointer
    `invokeMethod` that looks identical until the day it loses the race.

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
9. **The three cases are `RUN_SERIAL` and OWN the `media/*` keys.** An earlier
   draft claimed the opposite — that driving the panel through verbs writes no
   settings — and it is false: §B.4 has the panel persist `media/lastSourceId`,
   `media/lastPath/<sourceId>`, `media/categoryMask` and
   `media/searchRecursive`, and the verbs drive the REAL panel (T10), so every
   one of those writes fires. That is exactly `SStdMixerView::saveTrackControlWidth`'s
   shape, which the audited `smaragd.ini` row already names as the residual
   hazard. Each case therefore declares in its header that it owns those keys,
   restores them, and leaves the INI **byte-identical** across a full `-j4` run
   (md5, stated in the PR body). Suppressing persistence under `--test-case` was
   considered and rejected: it would make the gate test something other than the
   shipping code.

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
5. Closing **or switching** the project while a placement is pending cancels
   it: nothing is placed. There is no sanitizer build in this repo, so "no
   crash" here is an assertion of observed behaviour and not of memory safety —
   say so in the PR body rather than letting a green run imply more.
5a. **A pending placement whose target track was deleted mid-fetch places
   NOTHING** and reports it, rather than resolving a stale index-path onto
   whichever track now sits at those indices (§B.5). Delete the track, let the
   delayed source complete, assert the clip count on every track is unchanged.
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
main/testkit/src/swebdavstub.{h,cpp}      QTcpServer speaking PROPFIND+GET
main/media/tests/webdav_source_test.cpp   links the stub from testkit
```

**GATE 4 IS UNIT-TEST ONLY. Its two qxa cases have moved to gate 5c**, and this
is a correction to an earlier draft that could not have been built. Those cases
claimed to "drive the REAL panel against a stub started by the case" — but a
`.qxa` runs inside `smaragd.exe`, the stub was placed in `main/media/tests`
where only the unit-test binary links it, **no verb existed to start it**, and
registering a WebDAV source needs a URL and a credential whose only entry points
(the accounts page, `CredentialProvider`) do not ship until gate 5. It was a
gate with a hidden forward dependency on the next one.

The fix is two moves. The stub lives in **`main/testkit`** — which the app
already links, so both the unit test and a running `smaragd.exe` can start one —
and the end-to-end cases land in gate 5c behind a new `media-webdav-stub` verb,
once accounts exist to point at it. Gate 4 then has no forward dependency and no
end-to-end coverage; gate 5c supplies the latter one PR later.

**The stub is the gate.** A `QTcpServer` on `127.0.0.1:0` that answers
`PROPFIND` with a canned multistatus body built from a table and `GET` with the
bytes of a fixture WAV, and that can be told to answer `401`, `404`, `500`, to
stall, or to close mid-body. Plain HTTP, no TLS — TLS is Qt's code, not ours,
and a self-signed certificate in the repo would be a liability. That leaves
"TLS error surfacing" as manual (§C.6). Binding `127.0.0.1:0` (an
OS-assigned port) is what keeps it safe under `ctest -j4`: no fixed port, so
four concurrent cases cannot collide.

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
8. `ignoreSslErrors` appears nowhere: `grep -rn "ignoreSslErrors" smaragd/` is
   empty.
9. End-to-end coverage is **deferred to gate 5c** and the PR body says so
   plainly: at the end of gate 4 the WebDAV client is unit-tested and has never
   been driven from the app.

**Do not touch.** The panel's UI code (the source is chosen by id; the panel is
already source-agnostic from gate 2). The cache.

---

### GATE 5 — the secret store, accounts, and the Options page

**Deliverable.** A user can add a Nextcloud account in Edit → Options → Media,
test it, and see it in the browser's source picker — with the password
**encrypted at rest** by the platform's own credential protection.

**This gate is THREE PRs, not one**, and the split is prescribed rather than
offered — it is the largest gate and it absorbed gate 4's end-to-end cases:

- **5a — `SSecretStore` + `secret_store_test`.** No UI at all, no media code.
- **5b — the accounts model, the Options → Media page, `assert-media-options`.**
- **5c — the end-to-end WebDAV cases** (`media_webdav_browse.qxa`,
  `media_webdav_drop.qxa`) behind a new **`media-webdav-stub`** verb that starts
  the gate-4 stub in-process, now that an account exists to point the panel at.
  This is the coverage gate 4 deliberately does not have.

**Files.**

```
main/shell/include/app/shell/ssecretstore.h    the backend seam (§B.8)
main/shell/src/ssecretstore.cpp                dispatch + memory + none
main/shell/src/ssecretstore_win.cpp            DPAPI          (crypt32)
main/shell/src/ssecretstore_mac.mm             Keychain       (Security.framework)
main/shell/src/ssecretstore_linux.cpp          libsecret, behind TW_HAVE_LIBSECRET
main/testkit/src/swebdavstub.{h,cpp}           MOVED HERE from gate 4
main/shell/tests/secret_store_test.cpp         ctest target
main/media/include/app/media/smediacredentials.h   the provider INTERFACE
main/servicesui/src/soptionsdialog.cpp         a new "Media" page
main/servicesui/include/app/servicesui/soptionsdialog.h  + describeMediaPage()
main/shell/include/app/shell/ssettings.h       account accessors (§B.8)
main/testkit/src/smediatestactions.cpp         + assert-media-options
tests/cases/media_options_page.qxa             5b. RUN_SERIAL, owns its keys
tests/cases/media_secret_redaction.qxa         5b. RUN_SERIAL
tests/cases/media_webdav_browse.qxa            5c. from gate 4
tests/cases/media_webdav_drop.qxa              5c. from gate 4
docs/ACTIONS.md                                + the verb row
```

**`SSecretStore` lives in `main/shell`, next to `SSettings`** — machine-local
configuration is already that module's job, and a credential store is not
media-specific. `main/media` therefore never sees a secret store: it declares
`smedia::CredentialProvider` (an interface: "give me the `Authorization` header
value for this source id") and the **shell installs an implementation on the
registry at startup**. That is `SAppContext`'s exact shape — the lower module
declares the seam, the composition root implements it — and it keeps the layer
boundary of §B.1 intact.

**`SWebDavClient` takes an `Authorization` HEADER VALUE, not a user/password
pair.** Basic is then `"Basic " + base64(user:secret)` computed by the provider,
and a future Login Flow v2 token is `"Bearer …"` with no change to the client
(§B.8a).

The page mirrors the MIDI page's build/load/apply triple (servicesui CONTRACT
inv. 7): a list of accounts, Add/Edit/Remove, a per-account form of URL,
username and app password, a **Remember password** checkbox, a line naming the
**backend actually in use** ("stored with Windows DPAPI, protected by your
login" / "this system has no credential store — the password is kept for this
session only", the latter with **Remember disabled**), and a **Test connection**
button reporting the HTTP status.
`assert-media-options` builds the REAL `SOptionsDialog` off screen and matches
`describeMediaPage()`, exactly as `assert-midi-options` does.

**ACs.**

*The store (`secret_store_test`, every platform):*

1. Store → retrieve → delete round-trips for every backend the platform
   compiles, driven through `SMARAGD_SECRET_BACKEND`; a retrieve of an unknown
   key returns "unset", never an empty string that could be sent as a password.
2. A secret containing non-ASCII, embedded NULs, and 4 KB of data round-trips
   byte-exactly. (An app password is ASCII; a future token need not be.)
3. **The ciphertext is not the plaintext**: for `dpapi`, the stored INI value
   contains neither the plaintext nor any 8-byte substring of it, and differs
   across two stores of the same secret (DPAPI salts).
3a. **`none` persists nothing and disables Remember** — no INI key, no keychain
   item, and the store reports that it cannot remember rather than appearing to
   succeed and losing the secret at exit.
4. **A corrupt / foreign blob is `undecryptable`, never garbage.** Overwrite
   `passwordEnc` with random base64 and with a blob from a different scheme tag:
   both report `undecryptable`, both log a warning, and neither yields a string
   a caller could send (§B.8 rule 1, T14).
5. A scheme tag naming a backend this build does not have reports
   `undecryptable` and names the scheme in the log — it does not fall back to
   another backend and try to decrypt.
6. `SMARAGD_SECRET_BACKEND=memory` writes **no INI key and no keychain item**;
   asserted by an mtime + md5 check on the INI and by the absence of the
   keychain service.
6a. **`secret_store_test` NEVER touches the user's real `smaragd.ini` or real
   keychain.** ACs 3-5 read and corrupt `passwordEnc` values, and going through
   the `SSettings` singleton would mutate the developer's own INI **from a ctest
   unit test running concurrently with the qxa suite** — breaking the audited
   byte-identical-INI property from a test that declares no `RUN_SERIAL` and has
   no way to. The test constructs its own `QSettings` over a temp file and
   passes it in; the store takes its settings object rather than reaching for
   the singleton. Same for the keychain: `SMARAGD_SECRET_BACKEND` selects, and
   the real-backend ACs run against a test-scoped service name.

*The accounts page:*

7. An account round-trips: add, restart, still listed, and the browser's source
   combo offers `nextcloud:<accountId>`.
7a. **The options-page cases name their backend explicitly.** Rule 5 makes
   `memory` the `--test-case` default, but ACs 8 and 17 need a backend that
   actually persists, so `media_options_page.qxa` sets
   `SMARAGD_SECRET_BACKEND=dpapi` (or the platform's real one) in its CTest
   environment — and is `RUN_SERIAL` and restores its keys precisely because
   that writes real ciphertext into the shared INI. An earlier draft left this
   implied; implied is how the `-j` contract gets broken.
8. **No plaintext password key is ever written.** `grep` the INI for the known
   test password after a save with Remember ON: absent. `passwordScheme` is
   present, `passwordEnc` is present (or absent with a keychain backend), and
   `media/nextcloud/<id>/password` **does not exist**.
9. With **Remember off**, neither `passwordEnc` nor `passwordScheme` is written,
   and the source is usable for the rest of the session.
10. The dialog names the backend in use, and offers Remember **only** when a
    real store is present — with `SMARAGD_SECRET_BACKEND=none` the checkbox is
    disabled and the reason is in `describeMediaPage()`
    (`assert-media-options contains=`).
11. Removing an account removes every one of its keys **and its stored secret**
    (a keychain item left behind is a leak that outlives the app) and closes any
    open source for it.
12. **Test connection** against the gate-4 stub reports success; against a `401`
    it reports the status text and does not save a broken account silently.
13. An account whose URL is not parseable is refused at the dialog, not at the
    first request.

*Redaction (`media_secret_redaction.qxa`):*

14. A case sets a password of a known, unusual literal, drives a browse and a
    failing request against the stub, and asserts BOTH `assert-log
    contains="<that literal>" maxCount="0"` **and** `contains="<base64 of
    user:literal>" maxCount="0"` — a Basic header leaks the base64, not the
    password, so the second assertion is the one that actually bites. It
    deliberately does NOT assert `"Basic "` absent: that would forbid the
    correctly redacted `Authorization: Basic <redacted>` line (§B.8 rule 2, T13).
15. `describeMediaPage()` reports `password=set|unset|undecryptable` and the
    literal appears nowhere in it.

*Hygiene:*

16. Both new cases are **`RUN_SERIAL`**, declare in their headers that they OWN
    the `media/nextcloud/qxatest/*` keys, restore them, and leave `smaragd.ini`
    byte-identical — verified with an md5 across a full `-j4` run and stated in
    the PR body (T6, the `midi_options_page` precedent).
17. A plaintext `media/nextcloud/<id>/password` planted by the test is migrated
    on load: re-stored through the store and the plaintext key **removed**.

*End to end (5c):*

18. `media-webdav-stub` starts a stub on `127.0.0.1:0` and reports its port; an
    account pointed at it appears in the source combo, browses, and searches —
    the panel driven by the gate-2 verbs, unchanged.
19. A drop from that source places a clip whose rendered audio matches the
    fixture's RMS: the full chain, PROPFIND → GET → cache → project copy →
    `add-sample` → a sounding clip. This is the first time any of it runs
    inside the app.

**Do not touch.** The provider layer's ABI. The cache. `SSettings`'s existing
keys.

**Ungated and named in the PR body:** whether DPAPI/Keychain/libsecret actually
resist an attacker — the ACs verify the plumbing and the failure modes, not the
cryptography, which is the platform's. Real-keychain behaviour on macOS
(prompting, locked keychains, first-unlock) is manual, per §C.6.

---

### GATE 6 — closeout: contracts, docs, and the manual runbook

**Deliverable.** The feature is documented where this repo documents things,
and the parts no headless gate can reach have a runbook.

**Files.**

```
main/media/CONTRACT.md          completed (all invariants, all knobs)
main/mediabrowser/CONTRACT.md   completed
main/timeline/CONTRACT.md       + the `media:` drop branch invariant
main/shell/CONTRACT.md          + the 7th dock, + SSecretStore's invariants
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
and a server with a self-signed certificate. It also covers the **credential
store on real hardware**: the password survives a restart, a `smaragd.ini`
copied to a second machine reports `undecryptable` and prompts rather than
sending anything, and on macOS a locked keychain is handled without a hang.

**ACs.**

1. Every invariant asserted in §B appears in a `CONTRACT.md` next to the code
   that must uphold it.
2. `docs/ACTIONS.md` has a row for all seven new verbs with their real
   attribute defaults, and `action_roundtrip_test` covers each (it enumerates
   the registry, so a verb missing `writeXml`/`readXml` symmetry fails).
3. The CLAUDE.md section states, in the house's own idiom, the four things a
   newcomer will otherwise get wrong: placement is always `add-sample` over a
   local path (§B.5); a cache path in a saved project is not portable, hence
   the project copy (§B.6/T11); results are dropped by request id, not by
   winning a cancel race (§B.2 inv. 2); and a secret goes through
   `SSecretStore`, never into the INI, a log or a `describe()` — with
   `SMARAGD_SECRET_BACKEND=memory` as the `--test-case` default listed beside
   the other backend knobs (§B.8).
4. The manual runbook exists and is precise enough to follow. **Running it is
   the AUTHOR's step, not a subagent's** — it needs a real Nextcloud server and
   real credentials, which is exactly how `docs/ASIO_WINDOWS_GATE.md` is
   structured (Phase 1 landed with the gate run PENDING and said so). The gate-6
   PR may therefore land with the runbook unrun, provided it says so.
5. **CLAUDE.md's stale module line is fixed on the way past.** It states the app
   is "ONE OBJECT library (`smaragd_app`)"; the tree has had four
   (`app_model < app_core < app_objects < app_ui`) since Phase 6, and an agent
   reading that line looks for a structure that does not exist. Not this
   proposal's doing, but this proposal is the next thing to edit that file.
6. `plan/STATE.md` carries the chronological record, including anything the
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
the accounts page's load/apply; and the secret store's round-trip, its
"never plaintext in the INI" rule, its `undecryptable` failure modes and its
redaction from the log.

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
- **Whether the platform credential stores actually resist an attacker.** The
  ACs gate the plumbing and the failure modes; the cryptography is DPAPI's,
  Keychain's and libsecret's, and this proposal does not audit it. Real-keychain
  behaviour on macOS — prompting, a locked keychain, first-unlock — is manual
  (every headless run uses `SMARAGD_SECRET_BACKEND=memory`).

---

## E. Risks

1. **Qt6::Network is the first HTTP client in this tree.** Nothing else depends
   on it, so the blast radius is contained to `main/media`, but it also means
   there is no house precedent for reply lifetime, proxies or TLS. §B.7's four
   rules and gate 4's ACs are the precedent being set. *Mitigation:* the stub
   server makes the client's behaviour observable without a server.
2. **Credential storage.** Encrypted at rest through the platform's own store
   (§B.8), so no plain text is written anywhere. Two residual exposures are
   named rather than papered over: a platform with no credential store cannot
   remember a password at all (session-only, Remember disabled — there is
   deliberately no weak tier to reason about), and HTTP Basic
   still puts the password on the wire on every request — which is what a token
   flow fixes, and why §B.8a puts Nextcloud **Login Flow v2** next rather than
   OAuth2. A revocable app password is the mitigation in the meantime.
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
| **Nextcloud Login Flow v2 / OAuth2** | The direction, and §B.8a is the analysis: Login Flow v2 first (browser login against the user's own server, SSO and 2FA included, a revocable app password back, **no client registration**), OAuth2 only where a deployment demands it and an admin can register a client. The MVP's job is not to block it — `SSecretStore` and an `Authorization`-header-taking client are what discharge that. |
| **libsecret as a hard dependency** | Optional (`TW_HAVE_LIBSECRET`). A Linux build without it cannot remember a password: session-only, Remember disabled, reason shown. |
| **Any encrypt-it-ourselves fallback** | Rejected outright (§B.8): it needs a cipher this tree does not link, and a scheme that documents itself as protecting nothing is a checkbox, not a control. |

---

## G. Two decisions, resolved by the requester (2026-08-18)

1. **The resources dock stays, unchanged and separate.** The adversarial review
   asked whether `SExternFileList` should be subsumed. The requester's answer:
   it is **a debugging / housekeeping panel that is rarely opened** — a project
   inventory with ref counts and a Cleanup entry point — and the media browser
   is a working tool for getting material INTO a project. Those are different
   jobs for different moments, and merging them would put housekeeping in front
   of someone reaching for a kick drum. So: two docks, deliberately; **no "This
   project" source** in the browser; no rename; `SExternFileList` is not touched
   by any of the six gates. The overlap is one shared payload spelling
   (`file:<abspath>`), which is a feature — it is why gate 2 needs no timeline
   change at all.
2. **`.oga` is OUT, and it was never requested.** The requester's brief said
   "mp3/mp4/ogg/wav/aiff"; `oga` was introduced by this document's own §B.3 and
   is simply a second spelling of Ogg, which `ogg` already covers. `kAudio` is
   the Insert-sample dialog's exact seven and the two can never disagree.
   Adding a suffix later is a deliberate, user-visible change to BOTH — which is
   the point of there being one list.

---

## H. Summary of the six gates

| Gate | Ships | Gated by |
|---|---|---|
| 1 | `main/media`: the ABI + the local provider, async, bounded, id-tagged | `media_source_test` over a committed fixture tree |
| 2 | The dock: source picker, tree, filter, incremental search, drag out (local) | 3 qxa cases through the REAL panel and the REAL drop handler |
| 3 | The cache, the `media:` payload, deferred placement, project-relative copy | `media_cache_test` + a deferred-drop case + a two-process cache race |
| 4 | The Nextcloud/WebDAV connector — **unit-tested only, no app coverage** | `webdav_source_test` against the in-repo stub (which lives in `main/testkit`) |
| 5 | **Three PRs.** 5a `SSecretStore` (DPAPI / Keychain / libsecret / none); 5b accounts + the Options → Media page; 5c the end-to-end WebDAV cases gate 4 could not host | `secret_store_test`; `media_options_page` + `media_secret_redaction`; `media_webdav_browse` + `media_webdav_drop` (all RUN_SERIAL) |
| 6 | Contracts, docs, CLAUDE.md, STATE.md, the manual runbook | the runbook, run once and recorded |
