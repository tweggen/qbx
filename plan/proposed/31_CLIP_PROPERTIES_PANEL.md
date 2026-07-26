# Proposal 31: Clip properties panel

> **Status: EXECUTED (2026-07-26).** See `plan/STATE.md` for the execution
> record, including two latent `SName` bugs found on the way (the setter stored
> the empty string for every input; the field was never serialized).
>
> One dockable panel that is the single place to
> inspect and edit the selected clip(s), opened with F2 or from the clip context
> menu. Absorbs the per-clip property items currently scattered across the
> arranger context menu and the Test menu. Pure UI + one new action; no engine,
> no threading, no change to a single rendered sample.

## A. What is wrong today

### A.1 Per-clip properties live in three unrelated places

`SMVActualView::ctGlobalShow` (`main/timeline/src/sstdmixerview.cpp:603`) builds
**one flat menu** that mixes clip properties, structural clip operations, track
operations and global operations. Its clip section is:

| Item | Site | Kind |
|---|---|---|
| Split object | `:607` | structural |
| Pitch up / down (semitone) | `:608`, `:609` | **property** |
| Remove loop | `:612-617` | **property** |
| Preserve formants | `:620-633` | **property** |
| Add link | `:634` | structural |

Numeric entry for the same properties exists only as two `QInputDialog` prompts
buried in the **Test** menu (`main/shell/src/smainwindow.cpp:906-907`, with the
slots at `:1352` and `:1372`). One of them is worse than merely misplaced:

```cpp
// smainwindow.cpp:1365 — runSetClipStretch
cut->setStretch( s );      // NOT an action: not undoable, not scriptable
```

That is the last per-clip property write in the app that bypasses `SAction`,
against `main/actions/CONTRACT.md` invariant 1 ("every user-visible mutation is
an SAction").

### A.2 The menu ignores multi-selection

`ctGlobalShow` and every slot it wires act on `lastClickSLink_` alone
(`sstdmixerview.cpp:894-895`). `nudgeClipPitch` (`:487-556`) is the sole
exception — it prefers `getCurrentSelectionPaths()` and falls back to the
clicked clip. So selecting five clips and toggling *Preserve formants* silently
changes one of them.

### A.3 Several properties are gesture-only

The slip anchor, the clip duration, the loop-segment length and the stretch
factor can be reached **only** through mouse drags. There is no way to type an
exact value, and `SCut::getDetailEditWidget()` returns `NULL`
(`main/objects/cut/src/scut.cpp:613`), so the object model's own detail-editor
hook is unimplemented for clips.

### A.4 Two smaller truths worth recording

- `set-formant-preserve` is registered in code
  (`ssetformantpreserveaction.cpp:104-109`) but **missing from `docs/ACTIONS.md`**
  — the table jumps `select-take` → `screenshot`. It also has no `.qxa` case.
- `SObject` stores and serialises `volume`/`pan`/`delay`/`SName`/`muted` on
  every `SCut` (`main/model/src/sobject.cpp:104-128`), but **nothing consumes
  the first three at clip level** — `twTrackMix` reads volume from the *track*
  (`main/objects/mixer/src/sstdmixer.cpp:331`). They are latent storage, not
  working properties.

## B. Design

### B.1 The dock — one window, remembered placement

A fourth `QDockWidget` created in the `SMainWindow` ctor beside the existing
three (`smainwindow.cpp:914-942`), following that pattern verbatim:

```cpp
qDockClipProps_ = new QDockWidget( tr( "Clip Properties" ), this );
qDockClipProps_->setObjectName( "dock_clip_properties" );   // load-bearing
clipPropsPanel_ = new SClipPropertiesPanel( qDockClipProps_ );
qDockClipProps_->setWidget( clipPropsPanel_ );
addDockWidget( Qt::RightDockWidgetArea, qDockClipProps_ );
qDockClipProps_->hide();          // first run only; restoreState() overrides
```

**Remembering the docked-vs-floating state needs no new code and no settings
key.** The `objectName` is what `QMainWindow::saveState()`/`restoreState()` key
on, and those are already persisted to `ui/windowState` — written in
`closeEvent` (`smainwindow.cpp:522-538`), read by `restoreWindowLayout()`
(`:378-385`). Qt's blob carries the dock area, the floating flag, the floating
geometry and the visibility. Within a session, hiding and re-showing a dock
preserves the same state.

Two consequences to respect:

- The restore ordering invariant stands — `openMostRecent()` →
  `restoreWindowLayout()` → `show()` (`main/shell/src/main.cpp:312-337`,
  `main/shell/CONTRACT.md:27`). The dock is therefore created in the ctor, not
  lazily on first F2.
- A `ui/windowState` blob written by an older build has no entry for
  `dock_clip_properties`, so on first upgrade the dock keeps its ctor defaults
  (right area, hidden). That is the intended first-run behaviour, not a bug.

`toggleViewAction()` goes in the View menu (`smainwindow.cpp:944-957`) with the
text "Clip &properties" and **no** shortcut — F2 is a separate action so it can
raise and focus rather than blind-toggle.

### B.2 F2

There is no keybinding system in this codebase: shortcuts are hardcoded
`QAction::setShortcut` calls, and a grep for `QShortcut` / keymap / `shortcutFor`
returns nothing. F2 itself is unused anywhere in the repo.

To make it a *default* rather than a constant, the sequence is read once from the
existing option registry — `SOpt::ShortcutClipProperties =
"ui/shortcuts/clipProperties"` (`main/servicesui/include/app/servicesui/soptions.h`)
with default `"F2"` (`soptions.cpp`). Rebindable by editing the INI; no
keybinding UI is proposed here.

The action is registered window-wide with a bare `addAction()`, the idiom already
used for `actGotoStart_` (`smainwindow.cpp:805-806`, `:862`). Its slot shows,
`raise()`s (which pulls the dock out of a tab group) and focuses the first field.
If the dock is already visible **and** already holds focus, it hides — so F2
round-trips.

The clip context menu gains **"Clip &properties…"** as the first item of its clip
section, which selects the clicked clip if it is not already selected and then
triggers the same slot.

### B.3 Following the selection without a new signal

Selection state is `QList<SLink*>` on `SApplication` (`sapplication.h:28`,
`:147`). **There is no `selectionChanged` signal and none should be added.**
Every selection change is itself an `SAction`, so it goes
`submitAction` → `SActionHistory::drain_()` →
`project->notifyArrangementChanged()` (`main/actions/src/sactionhistory.cpp:66`).
That is already how the arranger learns to repaint
(`sstdmixerview.cpp:3004-3006`) and how clip renderers read selection state
pull-style (`main/objects/track/src/strackrndrinline.cpp:57`).

So the panel connects to `SProject::arrangementChanged`, mirroring
`attachTrackDetail()` / `detachTrackDetail()` (`smainwindow.cpp:110-137`)
including its rule of releasing raw pointers into the object graph **before** the
project is deleted:

```cpp
clipPropsConn_ = connect( currentProject_, &SProject::arrangementChanged,
                          clipPropsPanel_, &SClipPropertiesPanel::refresh );
```

This covers non-selection updates for free: an edge drag, an undo, or a `.qxa`
script all end in `arrangementChanged`.

`refresh()` **re-reads `getSelectionList()` every time and caches no `SLink*`.**
Selected links can be destroyed at any moment — `SApplication` auto-connects
`destroyed()` → `unselectSLink()` (`sapplication.cpp:126`) — and a cached
`SLink*` is exactly the use-after-destruction class of bug the `~SLink` fix
(2026) already cost us once. Each refresh resolves `SLink → SCut` afresh, through
`STakeStack::activeCut()` for stacks, the same resolution
`ctToggleFormantPreserve` performs (`sstdmixerview.cpp:588-593`).

### B.4 Contents, and the multi-selection rules

A `QFormLayout` in a `QScrollArea`, four group boxes. Every editable field
commits through an action that **already exists**, except Name.

**Read-only.** Source file name (`cut->getContent()` → `SExternFile::getFileName()`,
the pattern at `sstdmixerview.cpp:429-432`; container-backed cuts read "(asset)");
take-stack position; warp-anchor count with a **[Clear warp]** button
(`SResizeClipAction::setWarpAnchors` with an empty vector).

**Editable.**

| Field | Widget | Action | Greyed out on multi-select |
|---|---|---|---|
| Name | `QLineEdit` | **new** `set-clip-name` | **yes** |
| Start time | `QSpinBox` | `resize-clip` | **yes** |
| Duration | `QSpinBox` | `resize-clip` | no |
| Slip / srcStart | `QSpinBox` | `resize-clip` | no |
| Stretch | `QDoubleSpinBox` 0.1–10, 4 dp | `resize-clip` | no |
| Pitch (cents) | `QSpinBox` ±`SCut::PITCH_CENTS_LIMIT` | `set-pitch` | no |
| Loop length | `QSpinBox` + [Clear loop] | `resize-clip` | no |
| Preserve formants | tri-state `QCheckBox` | `set-formant-preserve` | no |

Name and Start time are the two that "do not make sense to edit for all of them":
a single absolute start position cannot mean anything for N clips, and a shared
name is not a property, it is a collision.

`SResizeClipAction` takes the whole window at once
(`sresizeclipaction.h:24-27`), so each field reads the other four off the clip
and passes them through unchanged — precisely what `ctRemoveLoop()` already does
(`sstdmixerview.cpp:568-580`). Both `resize-clip` and `set-pitch` handle take
stacks and edit-group broadcast internally, so the panel passes `take = -1`
(active take) and leaves `broadcast` at its default.

**Multi-clip semantics: absolute set, blank when mixed.** Matching `set-pitch`,
which `docs/ACTIONS.md` already documents as ABSOLUTE.

- *Display*: values agree → show; disagree → `blockSignals(true)` then
  `spin->lineEdit()->clear()`, so the field reads empty.
- *Commit*: on `editingFinished` only, ignoring an empty field; build one
  `SCompositeAction` holding one per-clip action and submit that, so **N clips
  are one undo step**. This is `nudgeClipPitch`'s shape
  (`sstdmixerview.cpp:531-546`), including its skip-if-unchanged filter — a
  rejected `apply()` increments `rejectedCount()` and fails headless tests, so
  no-op actions must never be submitted.
- *Formants*: `setTristate(true)`, `PartiallyChecked` when mixed, wired to
  **`clicked(bool)` rather than `stateChanged`** — `clicked` fires only on real
  user interaction, so a `refresh()` can never re-trigger a commit.

### B.5 Two pitfalls designed around up front

**Refresh-while-editing.** `refresh()` fires on every committed action —
including the one the panel itself just submitted. Guards: a `bool updating_`
re-entrancy flag around the whole refresh; `blockSignals()` while writing each
widget; commit only on `editingFinished`/`clicked`, never
`valueChanged`/`textChanged`; and never `setFocus()` from `refresh()`.

**Styling.** Do not copy `STrackDetailPanel`'s
`setStyleSheet("QWidget { … }")`-with-no-`paintEvent`: proposal 30 §B diagnoses
that as the stale/unpainted-region bug (Qt requires a `QWidget` subclass to draw
its own stylesheet background), and the unscoped selector cascades into every
child. Use no stylesheet (inherit the palette), or a class-scoped selector plus
`WA_StyledBackground` and a `PE_Widget` `paintEvent`. Likewise do not return a
large fixed `sizeHint()` for an empty panel (proposal 30 §C.7).

### B.6 The one new action

`SSetClipNameAction`, verb `set-clip-name`, attributes `clip`, `name`,
`take = "-1"`, `broadcast = "1"`. Structurally a copy of
`ssetformantpreserveaction.cpp` — same take-resolution helper, same edit-group
broadcast into an `SCompositeAction`, same old-value inverse, same static
registry initializer — with `SObject::getSName()/setSName()` (`sobject.h:337`,
`:349`) substituted for the formant accessors. `SName` is already serialised for
every `SObject`, so there is no persistence work.

### B.7 What leaves the menus

`ctGlobalShow`'s clip section becomes: **Clip properties…**, Split object,
Add link. Deleted: the pitch entries (`:608-609`), the Remove-loop block
(`:612-617`), the Preserve-formants block (`:620-633`), and the
`ctRemoveLoop()` / `ctToggleFormantPreserve()` slots.

`actPitchUp_` / `actPitchDown_` themselves are **kept** — they are created and
registered with `addAction()` at `sstdmixerview.cpp:3446-3477`, so `+`/`-` keep
transposing the selection with no change. Removing an item from a popup does not
unbind its shortcut. Editing gestures are likewise untouched.

The Test-menu entries `:906-907` and their slots (`:1352-1367`, `:1372-1395`)
are deleted, which also removes the app's last non-undoable clip-property write.

## C. Scope boundary: per-clip gain

Deliberately **not** in this proposal. `SCut` inherits `Volume`/`Pan`/`Delay`
storage and serialises them, but no clip-level DSP consumes them (§A.4), so a
volume slider here would be a control that does nothing. A real per-clip gain
needs a gain stage in the clip's reader chain, a `set-clip-gain` action, an
aspect-version bump and a `.qxa` energy test — its own proposal.

The panel is built so that a *relative*-mode numeric field (edit applies as a
delta to each selected clip, rather than an absolute set) drops into the shared
commit helper without restructuring, which is what a clip gain would want.

## D. Verification

Gates, per `CLAUDE.md`: `python tools/check_layering.py` (the panel lives in
`app/timeline`, which may already include `{actions, model, objects/*, pluginui,
servicesui, shell}` — no layering change needed), `python tools/check_logging.py`,
and the full qxa suite **run from `smaragd/tests/cases/`** (fixtures are
CWD-relative; running from `smaragd/` makes every RMS assert fail bogusly).

New headless case `tests/cases/clip_properties_actions.qxa`, modelled on
`warp_marker_actions.qxa` + `takes_serialize_roundtrip.qxa`: place a clip, apply
`set-clip-name` / `set-formant-preserve` / `set-pitch`, `save-project` +
`load-project` to prove the values survive, and `<verify-undo/>`. This also
closes the §A.4 hole — there is no qxa coverage for `set-formant-preserve` today.
`action_roundtrip_test.exe` picks up the new verb's XML round-trip automatically.

**Byte-exactness**: render a project before and after and `cmp` the WAVs (they
are 16-bit PCM — do not parse as float32). This work must not alter one sample.

Manual matrix:

1. F2 with nothing selected → panel opens showing "No clip selected".
2. One clip selected → fields populate; type stretch 2.0 + Enter → the clip
   doubles in length; Ctrl+Z restores it in **one** step.
3. Three clips with different pitches → pitch field blank, Name and Start greyed
   out; type `1200` → all three transpose; one Ctrl+Z undoes all three.
4. Toggle Preserve formants over a mixed selection → the checkbox leaves
   `PartiallyChecked`, all three end up equal, and the badge shows `F`
   (`main/objects/cut/src/scutrndrinline.cpp:96-124`).
5. Drag a clip edge with the panel open → duration/slip update on release, and a
   field being typed into is not clobbered (§B.5).
6. Float the panel, move it to the right edge, quit, relaunch → it reopens
   floating at that position. Close it, press F2 → same state.
7. Right-click a clip → the clip section shows only *Clip properties…*, *Split
   object*, *Add link*; `+`/`-` still transpose the selection.
