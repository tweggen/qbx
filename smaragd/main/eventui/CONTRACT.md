# app/eventui — CONTRACT

Purpose: the EVENT EDITOR — the piano roll and the widgets around it
(`SEventEditorDock`: toolbar + `SEventTimeRuler` + a view from the kind
registry; `SEventEditorView` + `SPianoRollView`; `SEventTimeAxis`), and the
`SVirtualKeyboardDock`. Proposal 36 P4, design §6.2 and §6.3.

Public headers: app/eventui/*.h

Depends on (engine): tw/core, tw/graph, **tw/events** (the project's
`twTempoMap`, and nothing else — this module never touches a page).
App edges: actions, model, objects/midi, objects/mixer, objects/track, shell
(per tools/check_layering.py). It sits at the RANK of `pluginui`.

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
the empty state), `action_roundtrip_test` rows for `virtual-key` /
`drag-note` / `assert-event-editor`.

Known debt:
- **CC lanes are draw-one-point, not curve editing.** A press in a CC lane
  sets one controller value at the snapped tick through `set-events`; there is
  no line tool, no ramp, no selection. Curve drawing arrives with the
  automation UI (proposal 36 P6), which needs the same gestures.
- **No zoom of its own.** The vertical key height is fixed (8 px) and the
  horizontal axis is the arranger's. Unlinking the axis works, but there is no
  UI to zoom the unlinked axis yet.
- **Nothing sounds.** The editor writes notes; hearing them needs the
  instrument slot (proposal 36 P3b). `virtual-key` therefore inserts rather
  than previews, and there is no note-preview-on-click.
- The `SStepGridView` (tracker) and score kinds named in design §6.2 are not
  built; the registry exists so they are additions rather than surgery.
