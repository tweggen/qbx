# app/mediabrowser — CONTRACT

Purpose: the Media Browser DOCK (proposal 38 gate 2) and nothing else — a
source picker, a media-type checkbox menu, an editable path, a debounced
search box with a recurse checkbox, a tree that serves as both the browse tree
and the flat search result list, an inline error banner with Retry, and a
footer carrying the counts, the truncation and the busy flag.

Public headers: `app/mediabrowser/smediabrowserpanel.h`.

Layer: **app_ui**, at the rank of `pluginui` / `eventui` — a UI slice hosted by
the shell. The provider layer (`app/media`, app_core) is a DIFFERENT module and
no widget may ever appear in it.

App edges: `media` (the provider ABI it drives), `shell` (`SSettings`),
`servicesui` (`SOpt` — the module that OWNS the per-user option table; a second
spelling of a key is how a setting silently stops round-tripping), and
`actions`, declared for gate 3's deferred placement. **It must never reach
`app/timeline`**: the drag payload is the whole interface between the browser
and the arranger, and a browser that knew what a lane looked like would have
started doing the arranger's job.

Engine edges: `tw/core`, `tw/graph` (the base every app module gets);
`tw/core/twlog.h` is the only one used.

Threading: main thread only. Every request the panel issues is answered off the
main thread BY THE PROVIDER and delivered back on it (app/media inv. 1); this
module starts no thread of its own and touches no other one.

## Invariants

1. **SUPERSESSION IS BY REQUEST ID, never by a cancel winning a race**
   (app/media inv. 2, design §B.2). The panel keeps `rootRequest_` — the id of
   the request it is DISPLAYING — plus one id per pending lazy expand, and
   `onEntriesReady` DROPS any batch tagged with anything else. Issuing a new
   root request replaces the id before the old walk can possibly answer, so a
   late batch from a superseded search cannot repaint over a newer one *by
   construction*. `cancel()` is also called, but only to save the walk: it is a
   courtesy, not the correctness property. A change that makes the drop
   decision consult a "was this cancelled?" flag instead of the id breaks this.

2. **NOTHING IS CONTACTED AT CONSTRUCTION** (design §B.4). The constructor
   REGISTERS the built-in local source (which does no I/O) and restores the
   remembered state into the controls, and stops there. A source is OPENED —
   `openCurrentSource()`, the one place a request is issued for a newly chosen
   source — when it is SELECTED: by the combo, by `selectSource()`, or by the
   dock first becoming visible (`showEvent`). A dock restored with
   `media/lastSourceId` pointing at a dead server therefore costs a banner at
   the next click, never a hang at launch. A headless run never shows the dock,
   which is what keeps a qxa case from opening a source no verb asked for.

3. **THE PANEL IS BUILT OFF SCREEN AND NEVER SHOWN under `--test-case`**
   (design trap T10), so every test verb pushes it its state explicitly and
   `describe()` is the oracle. Nothing here may depend on having been painted,
   on a layout having run, or on a widget having a real size.

4. **A LOCAL ROW EMITS `file:<absolute path>`**, under
   `application/x-smaragd-resource` — byte for byte the payload
   `SExternFileList::startDrag` emits and `SMVActualView::dropEvent` has always
   accepted (design §A.1). That identity is why gate 2 changes not one line of
   `app/timeline`. A DIRECTORY row is not draggable (`mimeForItem` returns
   null and the item loses `Qt::ItemIsDragEnabled`).

   **Since gate 3, a row whose source reports `NeedsFetch` emits
   `media:<uri>`** instead, and the arranger's one new branch hands that to
   `smediadrop::placeWhenLocal`. The fork is on the CAPABILITY, never on the id
   `"local"`: a source that does not report `NeedsFetch` already has its file on
   this file system, which is exactly what the `file:` branch wants, so a second
   local-shaped provider needs no entry in a list here.

4a. **THE PANEL FEEDS THE CACHE WHAT A LISTING KNEW** — `appendEntries` calls
   `SMediaCache::noteEntry()` for every row. A `media:` payload carries an
   `SMediaRef` and nothing else (that is what keeps the arranger's branch five
   lines), so this dock is the ONLY thing in the process that ever holds an
   entry's etag / mtime / size, and the cache's content key needs all three to
   be able to notice that a remote file changed. Dropping this call does not
   break anything visibly — it silently turns the cache into one that can never
   detect a remote edit.

5. **A BOUND IS ANNOUNCED** (app/media inv. 4). `truncatedCount > 0` on the
   final batch reaches the footer as "N items shown; at least M more were not
   (truncated)" and a `TW_LOGW`. Silent truncation reads as "covered
   everything".

6. **A FAILED REQUEST IS AN INLINE BANNER WITH RETRY**, never a modal and never
   an empty listing. An empty folder and an unreachable one must not look the
   same; `describe()` reports `banner=` and `error=` so a script can tell them
   apart too.

7. **THE FOUR `media/*` KEYS ARE WRITTEN BY THE REAL PANEL, INCLUDING UNDER
   `--test-case`.** `media/lastSourceId`, `media/lastPath/<sourceId>`,
   `media/categoryMask`, `media/searchRecursive`. Suppressing persistence in a
   test run was considered and rejected (design AC 9): it would make the gate
   test something other than the shipping code. The consequence is the one
   `midi_options_page` already carries — every qxa case that drives this panel
   is `RUN_SERIAL`, declares in its header that it OWNS those keys, and
   restores them, so `smaragd.ini` comes back with identical CONTENT.

   **CONTENT, not md5, and the distinction is not pedantry.** Gate 2 measured
   it: `QSettings` rewrites the whole file from its own in-memory map and does
   not promise to preserve section ORDER across processes, so the md5 of
   `smaragd.ini` can legitimately move while every key in it is exactly as it
   was — observed on `midi_options_page` alone, with no media case in the run.
   What holds is content identity plus the OWNERSHIP CONVENTION (one declared
   owner per key, `RUN_SERIAL`), and it is the convention rather than any
   locking that a future case must not quietly break: a case that READS a key
   another case owns is racing it, and `RUN_SERIAL` on the writer is what would
   have to be noticed.

8. **THE SUFFIX FILTER IS SOURCE STATE, and `smedia::kAudio` is its only
   source of truth.** One dock control governs browse and search alike; the
   panel never invents a suffix list. An EMPTY mask means "no filter" (show
   everything), not "show nothing" — the one place that decision is made is
   `smedia::suffixAccepted`.

9. **SIZING FOLLOWS `SExternFileList`'s POLICY, verbatim**:
   `QSizePolicy::Preferred/Expanding`, `minimumWidth 200`, `maximumWidth 360`.
   Its comment gives the reason and it applies here unchanged — a `QTreeWidget`
   is Expanding by default, so without the cap `QMainWindow` hands this dock
   all the resize space and pins the arranger at its minimum.

10. **A DOUBLE-CLICK ON A DIRECTORY NAVIGATES; IT DOES NOT EXPAND.**
   `onItemDoubleClicked` calls `setBrowsePath()` for a directory row and does
   NOTHING for a file row (a file is placed by dragging — §B.5 — and there is
   no audition in this MVP, so a second, invisible placement gesture would be
   a surprise, not a convenience). Two consequences that are the point rather
   than side effects: the PATH FIELD follows, because `setBrowsePath()` is the
   one place the field, the persisted `media/lastPath/<source>` and the
   re-listing are kept in step; and a SEARCH started afterwards starts THERE,
   because `refreshRoot()` passes `path_` to `source_->search()` — so "recurse"
   means "this folder and below", not "the whole source".
   `setExpandsOnDoubleClick(false)` is required, not cosmetic: the tree is
   repopulated by the navigation, so the default toggle would issue a lazy
   `listDirectory` for a row about to be deleted. The triangle still expands in
   place. Gated by `media_browser_browse` items (4a)/(4b), driven through the
   REAL `itemDoubleClicked` SIGNAL (`activateRowNamed` emits it on the widget),
   so removing the connection fails the case.

11. **The tree is ONE column (Name); size is a tooltip, not a column**
   (AC-d1, 2026-08-21). The dock is 200-360 px wide (inv. 9), and a Size
   column wide enough to read "12.3M" left too little for the name in a deep
   tree — so `makeItem()` puts the size on the Name item's tooltip instead
   (`Size: <mediaBrowserFormatSize()>`), and a directory (size unknown, -1)
   gets no size tooltip at all. `setIndentation(2 * fontMetrics().
   horizontalAdvance('M'))` replaces Qt's default per-level indent, which was
   wide enough to push a nested name off the right edge of a narrow dock well
   before the tree ran out of depth. `describe()`'s PER-ROW fields are
   UNCHANGED — it already read `kRoleSize`/`kRoleIsDir` item data, never
   column 1's text, so dropping the column moved no gate — but it grew an
   optional 4th field for the new tooltip (below), because otherwise nothing
   in the tooltip would be assertable at all.

## `describe()` — the format `assert-media-browser` matches

One state line, then one `name,size,dir[,tooltip=…]` row per row in
DEPTH-FIRST tree order (so an expanded directory's children follow it
immediately):

```
mode=browse|source=local|path=../media/lib|filter=audio|recursive=0|rows=3|truncated=0|busy=0|banner=0|error=
sub,-1,1
kick.wav,768044,0,tooltip=Size: 768k
snare.wav,768044,0,tooltip=Size: 768k
```

- `mode` is `browse` or `search`; `filter` is `smedia::categoryMaskToString`
  (`audio`, `midi`, `audio,midi`, or empty for "no filter").
- `size` is `-1` for a directory — the documented "unknown", never
  `QFileInfo::size()`, which is platform junk and would make an exact-count
  gate disagree between Windows and Linux.
- `tooltip=…` is APPENDED, never inserted, so it is a suffix on the
  `name,size,dir` triple and every existing `contains="name,size,dir"`
  substring match still matches (AC-d1, 2026-08-21). Present only on a FILE
  row (`makeItem()` sets no tooltip on a directory, whose size is unknown);
  its text is `Size: ` + `mediaBrowserFormatSize()`, the same spelling the
  removed Size column used to show.
- `busy` is 1 while a root request, a lazy expand, or a search still inside its
  250 ms debounce is outstanding. A test verb WAITS on it rather than sleeping.
- `path` is machine-dependent by nature; no committed case asserts it.

## Knobs

None of its own. The four `media/*` `SOpt` keys are the persisted state; their
names and defaults live in `app/servicesui/soptions.h`.

## How to test

`ctest -R "qxa.media_browser"` — three cases, all `RUN_SERIAL`:

| Case | What it pins |
|---|---|
| `media_browser_browse` | the exact row set under the Audio filter, the empty-mask positive control, two levels of lazy expand, and the banner on an unreachable path |
| `media_browser_search` | recursive vs non-recursive counts, the real 250 ms debounce, the return to browse mode, and SUPERSESSION (three searches back to back, the third's results stand) |
| `media_browser_drag_local` | the whole drag: the real `QMimeData`, the real `dragEnterEvent`/`dragMoveEvent`/`dropEvent`, a clip at frame 48000, the fixture's own RMS in the render, one undo, and the refusal of a directory row |

The verbs go through `SMainWindow` (`mediaBrowserSetSource` / `-SetPath` /
`-Search` / `-SetFilter` / `-Drag` / `describeMediaBrowser` /
`mediaBrowserBusy`), because `main/testkit` may not include `app/timeline` and
the drag has to reach both the panel and the arranger — the same route
`drag-clip-edge` and `assert-lane-alignment` already take.

**A synthetic drop is THREE events, not one.** Qt discards a bare
`QEvent::Drop`: `QApplication::notify` tracks the drag target a `DragEnter`
established, and a Drop with no active target never reaches `QWidget::event` —
measured, and true whether or not the widget is visible (a
`Qt::WA_DontShowOnScreen` `show()` does not help). `SMainWindow::mediaBrowserDrag`
therefore sends DragEnter, DragMove and Drop in order, which also makes the two
handlers that decide whether the arranger accepts this MIME type part of what
is gated.

## Known debt

- **The 250 ms debounce is gated, but the "typing four characters issues one
  walk" property is not** — a case drives one `setText`, not four keystrokes.
- **No pixel coverage.** The panel's `paintEvent` is never exercised: the
  `screenshot` verb grabs the screen's root window, blank under
  `QT_QPA_PLATFORM=offscreen`, and there is no verb that grabs this widget. The
  ghost pixmap in `SMediaBrowserTree::startDrag` is likewise unrun by any gate
  (`mediaBrowserDrag` builds the MIME without going through `startDrag`).
- **The dock's docked/floating/closed round trip is manual** (design AC 2):
  it rides entirely on `objectName` through Qt's opaque `ui/windowState` blob,
  and no verb can inspect it.
- **`SMediaBrowserTree::startDrag` and the panel's own mouse/keyboard gestures
  are unrun by any gate.** The verbs call the same `mimeForItem`, but a real
  drag begins in `startDrag`.
- **The Insert-sample dialog still builds its own filter string.** It is byte
  for byte `smedia::kAudio`'s seven suffixes and `app/media`'s CONTRACT says
  so, but the two are not yet ONE expression: unifying them needs the
  `timeline -> media` edge design §B.1 lists, and gate 2 was scoped to touch no
  timeline file. Adding a suffix therefore still means editing two places until
  that lands.
- Only `local` is registered. Gate 5 adds account sources, and the combo, the
  banner and the per-source path key are already shaped for them.
