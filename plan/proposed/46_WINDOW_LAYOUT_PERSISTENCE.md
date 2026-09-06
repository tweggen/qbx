# Proposal 46 — The window layout, and the per-track view state, survive a restart

**Status:** M1–M3 executed (branch `fix/window-layout-persistence`).

## The report

> "Currently the layout of dividers, and probably also the configuration which
> views are visible where is not saved (per user?)."

Both halves are true, and the cause is one line.

## What was verified before a line was changed

The persistence MACHINERY is complete and correct. Every dock carries an
`objectName` (`dock_extern_file_list`, `dock_track_detail`, `dock_log`,
`dock_clip_properties`, `dock_event_editor`, `dock_virtual_keyboard`,
`dock_media_browser`, `dock_feel_flow_puppet`) and so does every toolbar, and
`QMainWindow::saveState()` covers exactly what was reported missing: dock
visibility, dock area, floating state, tabification, toolbar placement **and the
separator (divider) positions**. `restoreWindowLayout()` reads it back in the
one order that works (`openMostRecent()` → restore → `show()`), with shell
CONTRACT inv. 4 fixing that order.

**It was written to disk from exactly one place** — `SMainWindow::closeEvent`.
And the app's own Quit never reached it:

```
smainwindow.cpp:1465  qFileMenu_->addAction( "E&xit", Qt::CTRL | Qt::Key_Q, this, SLOT( fileExit() ) );
smainwindow.cpp:823   void SMainWindow::fileExit() { ::exit( 0 ); }
```

On macOS Qt gives an action titled "Exit" the `QuitRole`, so ⌘Q **is** that
action: `::exit(0)`, straight out of the process, no `closeEvent`, no
`saveState()`, nothing written.

Two independent measurements on the reporting user's machine confirm it:

| Measurement | Result |
|---|---|
| `~/.config/Smaragd/smaragd.ini` | a `[ui]` section holding **only** `optionsLastPage` — no `windowGeometry`, no `windowState`. The layout had never once been saved. |
| `~/.config/Smaragd/smaragd.log` (4.8 MB) | **zero** occurrences of `"Smaragd exiting"`, the line printed after `app.exec()` returns. No session had ever ended through the normal path. |

**The same line cost two more things**, neither of them reported and both
worse than the reported symptom:

1. **Silent data loss.** `fileExit()` skipped `promptSaveUnsavedChanges()`, so
   ⌘Q on a project with unsaved edits discarded them with no dialog. This is
   the same class of defect `fix/editor-ui-and-shortcuts` fixed for the
   Save-then-Cancel path, reached by a different route.
2. **No orderly shutdown.** It skipped `smaragdOrderlyShutdown()` — the plugin
   scan thread was not stopped and the log's file writer was not flushed, which
   is why the log ends mid-session.

## M1 — one save method, and Quit uses the normal path

`SMainWindow::saveWindowLayout()` is now THE one place `ui/windowGeometry` and
`ui/windowState` are written, and it is called from three places:

- `closeEvent()`, **before** `closeProject()` — the existing comment is right
  and is now enforced by a flag rather than by call ordering alone: saving
  after the central widget is gone records a layout that does not round-trip
  through `restoreState()`.
- `QCoreApplication::aboutToQuit` — the belt-and-braces net for any quit route
  that does not close the window (a session logout, the macOS dock menu).
- the M2 autosave timer.

`fileExit()` is now `if( close() ) qApp->quit();`. ⌘Q therefore runs the
unsaved-changes prompt, saves the layout, and lets `app.exec()` return into
`smaragdOrderlyShutdown()` — and **cancelling the prompt now cancels the quit**,
which it could not before.

### `layoutFrozen_` — why a flag rather than an ordering rule

`closeEvent` sets `layoutFrozen_` immediately after its save, and
`saveWindowLayout()` refuses once it is set. Without it the `aboutToQuit`
handler would fire AFTER `closeProject()` has run and would overwrite the good
blob with one taken from a window that has no central widget — the exact
failure the closeEvent comment has warned about since it was written. The flag
encodes "the UI this blob describes no longer exists", which is the real
precondition; call ordering only encodes it by accident.

## M2 — surviving a crash, and never writing a headless run's layout

A separator drag emits **no Qt signal** — there is no `QMainWindow` layout-changed
notification of any kind — so nothing but a timer can catch one mid-session. A
120 s repeating timer calls `saveWindowLayout()`, which writes only when the
blob actually differs from the stored one (`saveState()` is cheap; the INI write
is the part worth avoiding). A crash or a force-quit now costs at most the last
two minutes of layout fiddling instead of the whole session.

**It is suppressed under `SApplication::isTestCaseMode()`, and so is every
automatic caller.** `ctest -j4` runs four processes against one shared
`smaragd.ini`, the suite's INI ownership convention is that a case declares the
keys it owns, and a headless run's geometry is junk — the main window is never
shown in a `--test-case` run at all, so a restored blob from one would place a
real session's docks from a window that was never mapped. The testkit verb
passes `force=true` to reach the method deliberately, and restores both keys
afterwards, so it leaves the INI exactly as it found it.

## M3 — the per-track view state moves onto the track

Even with M1 in place, four kinds of view state were still session-only, kept
in `STrack*`-keyed containers on the arranger:

| State | Was | Now |
|---|---|---|
| lane height scale | `SStdMixerView::trackScale_` (`QHash<const STrack*,double>`) | `STrack::laneHeightScale()`, attribute `laneHeightScale` |
| take-lane expansion | `SStdMixerView::takesExpanded_` (`QSet<STrack*>`) | `STrack::takesExpanded()`, attribute `takesExpanded` |
| shown automation lanes | `SAutomationLaneUi::shown_` (`QHash<const STrack*,QVector<SAutoLaneRef>>`) | `STrack::shownAutomationLanes()`, attribute `shownAutomation` |

This is not a new idea — it is the precedent `fix/track-list-polish (m)` already
set for the FOLD set, which was a `QSet<STrack*>` on the view and became
`STrack::isCollapsed()` for exactly these reasons:

- **It is a per-TRACK fact**, so it belongs on the track rather than in a
  second, view-owned copy.
- **Living on the object means it dies with the object**, which retires
  `SStdMixerView::pruneUiState()` and `SAutomationLaneUi::pruneTo()` entirely —
  and with them the dangling-`STrack*` hazard those walks existed to contain. A
  removed track could otherwise leave a key behind for a later track allocated
  at the same address to inherit; proposal 45 AC4.6 records that the master
  lane's UI state was being pruned on EVERY row rebuild because the walk did not
  reach system lanes, which would have looked like three separate bugs.
- **It survives a save/load round trip for free** through the ordinary
  attribute path.

**Every attribute is written only when it is not the default**, so every project
file written before this — and every committed golden — re-serializes
byte-identically. `laneHeightScale` is omitted at 1.0, `takesExpanded` at false,
`shownAutomation` when the list is empty.

`shownAutomation` is one attribute holding a semicolon-separated list, each
entry `target` or `target@slot`: `self:Volume;param:12@0`. A target spelling
(`self:…`, `param:<id>`, `cut:…`) contains no `;` and no `@`, so the encoding is
unambiguous, and `STrack::encodeShownAutomation()` / `decodeShownAutomation()`
are the ONE pair that spells it — the arranger converts to its own
`SAutoLaneRef` and never re-invents the encoding.

### What M3 deliberately does NOT do

**It does not mark the project dirty.** Dragging a lane taller changes a saved
property with no undo entry and no dirty flag, exactly as folding a track has
done since `fix/track-list-polish (m)` — so the state is written on the next
save the user makes for some other reason, and a session that only rearranged
lanes and quit still loses them. Making view gestures dirty the project is a
policy change (every fold, every lane drag would prompt on close), and it should
be decided for `collapsed` and these three together, not for three of the four.

## Gates

### Measured

| Sabotage | What failed |
|---|---|
| the serializer does not write the three M3 attributes | `lane_view_persists`: every `assert-lane-view` after the reload reads the defaults (`laneScale=1.000000\|takesExpanded=0\|automation=`) |
| the view ignores `STrack::takesExpanded()` when building rows | `lane_view_persists`: `assert-lane-alignment` reads **3 rows, expected 5**, twice |
| the `isTestCaseMode()` suppression is removed | `window_layout_save`: `force=false wrote=true expected false`, twice |
| `saveWindowLayout()` returns false unconditionally | `window_layout_save`: `force=true wrote=false expected true`, twice |

A re-save of an untouched project (`legacy_takestack_wrap.qxp` loaded and
written straight back out) contains **0** occurrences of `laneHeightScale`,
`takesExpanded` or `shownAutomation` — the non-default-only rule holds, so no
existing project file or golden moves. And the user's `smaragd.ini` md5 is
unchanged across `window_layout_save`, including across the two runs in which
it FAILED.

| Gate | What it bites |
|---|---|
| `qxa.window_layout_save` | `saveWindowLayout()` produces a non-empty blob under BOTH keys when forced, and **refuses** when not forced in a `--test-case` run (the M2 suppression). The verb restores both keys, so the case writes nothing durable. |
| `qxa.lane_view_persists` | M3's round trip: set a lane scale, expand takes, show an automation lane, save, load, and read all three back THROUGH THE ARRANGER — so it proves the view adopts the persisted value, not merely that the model holds it. |
| `action_roundtrip_test` | the new verb and the new `assert-lane-view` attributes |

**NOT gated, and said plainly:**

- **The ⌘Q / File→Exit wiring itself.** `fileExit()` now routes through
  `close()`, and there is no headless route to that: `promptSaveUnsavedChanges()`
  is a modal `exec()` and the quit would end the script's process. Hand-verified.
- **`aboutToQuit`.** Same reason.
- **The M2 timer firing.** What is gated is the method and its test-mode
  suppression; a 120 s wall-clock wait is not something to put in the suite.
- **That a restored layout LOOKS right.** The main window is never shown in a
  `--test-case` run, so no headless gate can see a placed dock. This is the same
  standing gap `docs/MEDIA_BROWSER_MANUAL_GATE.md` already records for the
  dock round trip, and it is unchanged by this branch.
- **Which editor TABS are open** (`SViewTabs`). Still session-only. It is
  per-PROJECT state, not per-track and not per-user, so it wants the `.qxp` and
  a decision about what happens when a tab's root is gone at load — a separate
  piece of work.

## A pre-existing defect this branch FOUND and did not fix

**A scripted `load-project` does not rebind the arranger.**
`SMainWindow::ensureArranger_()` returns the active tab's existing editor
whenever there is one, while `SLoadProjectAction` loads into the SAME `SProject`
and gives it a NEW root mixer ("a load REPLACES the project, but it loads INTO
this same SProject object"). So every arranger reach-through after a second
`load-project` in one script answers from the PREVIOUS load's mixer.

Measured while building the M3 gate: a `set-lane-view laneScaleRow="0"` issued
after a reload silently wrote to the old project's track and the model kept its
pre-reload value.

It is recorded (`main/shell/CONTRACT.md` inv. 60) rather than fixed because it
is out of scope and because the failure mode is the dangerous kind — it
silently WEAKENS assertions instead of failing. A row-count or zoom/pan check
placed after a reload measures the stale view and passes whatever the code
under test does. `qxa.track_list_view_roundtrip`'s post-load
`secondWidth`/`scrollX` assertions have exactly this blind spot today. Whether
the stale mixer is merely orphaned or actually freed was NOT established.
