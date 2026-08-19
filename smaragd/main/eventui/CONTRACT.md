# app/eventui — CONTRACT

Purpose: the EVENT EDITOR — the piano roll and the widgets around it
(`SEventEditorDock`: toolbar + `SEventTimeRuler` + a view from the kind
registry; `SEventEditorView` + `SPianoRollView`; `SEventTimeAxis`), and the
`SVirtualKeyboardDock`. Proposal 37 P4, design §6.2 and §6.3.

Public headers: app/eventui/*.h

Depends on (engine): tw/core, tw/graph, **tw/events** (the project's
`twTempoMap`, and nothing else — this module never touches a page).
App edges: actions, model, objects/midi, objects/mixer, objects/track,
servicesui, shell (per tools/check_layering.py). It sits at the RANK of
`pluginui`. `servicesui` (mouse-wheel pan/zoom, below) is read-only: `SOpt`
key names and defaults, never a widget from that module.

**It deliberately does NOT depend on `timeline`.** The editor mirrors the
arranger's zoom and scroll, but the wiring lives in the SHELL — the one module
that already sees both — so a 400-line piano roll does not acquire the
4000-line arranger as a dependency. `SMainWindow::linkEventEditorAxis()` is the
whole seam.

Invariants:

1. **Nothing here ever caches an `SLink*`, an `SMidiCut*` or an
   `SMidiSequence*`.** A view holds an index PATH and re-resolves it on every
   use (`SEventEditorView::resolve()`), because any action can destroy a
   selected link — the use-after-destruction class this codebase has been
   burned by. Same rule as timeline inv. 8, same reason.
2. **Every edit commits through a P1 verb, ONE action per GESTURE** (timeline
   inv. 3). A drag of N notes is one `set-notes`, not N `add-note`s; a
   quantize is one `quantize-notes`; a CC point is one `set-events`. The batch
   verbs are absolute, so the previous table IS the inverse.
3. **A live drag never touches the model.** It paints out of `SPianoRollView::
   drag_` and the release DISCARDS that preview and submits the action
   (revert-then-action). There is consequently nothing to roll back when a
   gesture is cancelled — the model was never wrong. Do not "optimise" this
   into a live model write; that is what makes a cancelled drag consistent and
   an undo step singular.
4. **The dock is a SELECTION FOLLOWER and must survive its own commit**
   (timeline inv. 9). `refresh()` hangs off `SProject::arrangementChanged`,
   which every committed action — the editor's own included — ends in; hence
   `updating_`, and hence no `setFocus()` from the refresh path. An audio-clip
   selection CLEARS the editor (`empty=1`) rather than leaving the previous
   MIDI clip on screen: an editor that lags the selection is how a user edits
   the wrong clip.

   That reactive connection is wired exactly ONCE, by
   `SMainWindow::attachEventEditor()` — which a HEADLESS test run never
   calls (`SActionRunner` sets the project straight on `SApplication`), and
   which even the interactive app only wires once a project exists. "Opening"
   the dock must therefore not rely on it alone: `SEventEditorDock::
   showEvent()` re-`refresh()`es whenever the dock actually becomes visible
   (best-effort — a hidden top-level window, which is what a headless run
   leaves `SMainWindow` as, delivers no `QShowEvent` to a still-docked
   child), and `SMainWindow::showEventEditor()` — the ACTIVE, TESTABLE half,
   used by the double-click-opens-the-editor gesture below — always
   `refresh()`es or `bindClip()`s explicitly rather than trusting either
   signal. The double-click handler works BECAUSE of Qt's own delivery
   order: `MouseButtonPress` (which already selects the clip through the
   arranger's ordinary single-click handler) always precedes
   `MouseButtonDblClick` — there is no SECOND press event to rely on — so by
   the time `mouseDoubleClickEvent` asks what is selected, it already is.
5. **`SEventTimeAxis` is the ONE px↔frame conversion**, and it is
   deliberately the SAME arithmetic as `SMVActualView::getXPosOfOffset` /
   `getTimeOf`, integer truncation included (timeline inv. 4). A "nicer"
   conversion here puts the piano roll's notes half a pixel away from the
   arranger's clip that contains them at some zooms. `leftPixels` is a PIXEL
   count (the arranger's `upperLeftX_`), not a frame.
6. **Ticks cross to frames only through the project's `twTempoMap` and the
   clip's own rate** (POSITION_DOMAINS rule 7). `SEventEditorView::frameOfTick`
   / `tickOfFrame` are the only place this module does that arithmetic, and
   `tickOfFrame` goes through `SMidiCut::timelineToSourceExact` so a round trip
   lands where `resize-clip`'s slip arithmetic would put it.
7. **One grid parser.** A division ("1/16", "1/8t") is parsed by
   `SQuantizeNotesAction::gridTicks()` — the same function `quantize-notes
   grid=` and (since P4) the arranger's `SSnapSpec` use. Three spellings of
   "an eighth-note triplet" is three chances to disagree.
8. **The kind REGISTRY is a static initializer** (`SEventEditorRegistry`,
   `SPianoRollView`'s `s_reg_pianoroll`), which is why `app_ui` must stay an
   OBJECT library — a STATIC one drops it silently and the dock comes up with
   no editor at all. Adding a tracker grid or a score view is a new file plus
   one registration; the dock never learns which view it is showing.
9. **The piano roll is ONE widget with three BANDS** (note grid / velocity /
   CC lanes), not three sibling widgets. Every band shares the one time axis
   and must stay column-aligned with the ruler and with the arranger above it;
   three layouts is exactly how that alignment drifts. `bandAt(y)` is the
   dispatch, and it is what makes `drag-note lane="velocity"` drive the same
   handlers a pointer does.
10. **The virtual keyboard must not steal Space.** Space is the transport,
    everywhere, always: an unmapped key is `ignore()`d so it reaches the
    shell's shortcut. Auto-repeat is dropped too — a held key is one note.
11. A gesture verb resizes the widget it drives before synthesising events
    (`SPianoRollView::tkDragNote`). A headless run never SHOWS the dock, so
    the view has whatever size the layout last gave it; the handlers work off
    the axis rather than off screen geometry, so growing it changes nothing
    but the hit-testable area. Same trick as `SStdMixerView::dragClipEdge`.

How to test: `piano_roll_edits.qxa` (virtual-key, the move / transpose /
resize / velocity-lane drags, each with an explicit `<undo count="1"/>` and
the prior assertion, plus the no-clip rejection), `event_editor_dock.qxa`
(the dock's `describe()`, the selection-follower clear on an audio clip, the
kind registry, quantize-from-the-toolbar, and PNG grabs of the piano roll and
the empty state), `event_editor_open.qxa` (double-click on an EVENT clip opens
the dock through the REAL gesture, the fixed 6 px row height, and the vertical
key scrollbar reaching both ends of 0-127), `action_roundtrip_test` rows for
`virtual-key` / `drag-note` / `scroll-event-editor-keys` / `double-click-clip`
/ `assert-event-editor`.

Known debt:
- **CC lanes are draw-one-point, not curve editing.** A press in a CC lane
  sets one controller value at the snapped tick through `set-events`; there is
  no line tool, no ramp, no selection. Curve drawing arrives with the
  automation UI (proposal 37 P6), which needs the same gestures.
- **Zoom exists on both axes now, but only through the wheel (mouse-wheel
  pan/zoom follow-up).** Horizontal zoom goes through the shared
  `SEventTimeAxis` (invariant 5); vertical zoom changes
  `SPianoRollView::keyHeight_` (default 6 px, was a FIXED constant before
  this) the same way the arranger's Ctrl+wheel changes track height. Neither
  has a dedicated numeric control (a spinbox, a toolbar zoom slider) and
  neither PERSISTS: a rebuilt `SPianoRollView` (switching editor kind and
  back) starts `keyHeight_` at the 6 px default again, and the axis is
  whatever the arranger last pushed if linked. A per-view, persisted zoom
  setting is still later work.
- **Nothing sounds.** The editor writes notes; hearing them needs the
  instrument slot (proposal 37 P3b). `virtual-key` therefore inserts rather
  than previews, and there is no note-preview-on-click.
- The `SStepGridView` (tracker) and score kinds named in design §6.2 are not
  built; the registry exists so they are additions rather than surgery.

12. **The virtual keyboard PLAYS as well as writes** (proposal 21 L2, design
    D9). `holdNote`/`releaseNote` send note-on/note-off on the computer
    keyboard's in-process MIDI port, which a live-armed instrument track hears
    exactly as it hears hardware; `pressNote` still writes at the locator. A
    mouse or key press does BOTH, in that order, and a `pressNote` that fails
    (no clip selected) must not stop the note from sounding.

    They go through `SApplication::keyboardNoteOn/Off`, NOT through
    `tw/devices`: this module's engine budget is `core`, `graph` and `events`
    (tools/check_layering.py), and routing the two calls through the shell is
    the same arrangement the arranger's zoom uses.

    EVERY exit from a key must release it - key-up, mouse-up and focus-out -
    or a note sounds forever. Auto-repeat is refused on BOTH edges: X11
    delivers a release before every repeated press, so a repeat read as a
    finger coming off the key would stutter the note.

13. **`SPianoRollView::wheelEvent` matches `SMVActualView::applyWheel`'s
    default gesture mapping and per-notch feel byte-for-byte** (mouse-wheel
    pan/zoom follow-up), through the piano roll's OWN `SOpt::Event*` keys
    (`main/servicesui/soptions.h`) — never the arranger's `SOpt::Wheel*` ones
    — so the two views can be retuned independently but open with the exact
    same behaviour. Plain wheel scrolls `keyScroll_` (the note grid's key
    rows); Shift+wheel pans the shared `SEventTimeAxis`; Ctrl and Ctrl+Shift
    zoom it (horizontal) or `keyHeight_` (vertical) — `SOpt::WheelAction` and
    its four values are reused verbatim, only the KEYS differ. The macOS
    accessibility-zoom early-out (physical Ctrl == `Qt::MetaModifier`) and the
    trackpad X→Y swap are duplicated from `sstdmixerview.cpp` rather than
    shared, because `eventui` may not depend on `timeline` (see "App edges"
    above) — a change to one must be checked against the other by eye.
    `main/servicesui/src/soptionsdialog.cpp`'s "Event Editor" page is the
    config UI, built/loaded/applied exactly like the arranger's "Mouse
    navigation" page. **No headless gate exists for either the gestures or
    the options page** (no verb drives a synthetic wheel event anywhere in
    this repo, and no verb builds the Options dialog off screen the way
    `assert-midi-options` does for the MIDI page) — hand-verified only.
