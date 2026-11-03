# app/samplerui — CONTRACT

Purpose: NassauDigital's EDITOR — one window per instance with four
fixed-size views (Key, Pad, Map, Zone), the hand-selection tap, the XML
command pane and the drop target (proposal 56 §4.11, milestone Q7;
nassau-digital `docs/DESIGN.md` §7, §9.3, §13 "Settled on 2026-11-03,
editor"; QBX-279).

Public headers: app/samplerui/*.h

Depends on (engine): tw/core, tw/graph, **tw/devices** (the `MidiInFanout`
sink of the hand-selection tap), **tw/sampler** (`twSamplerProgramModel`'s
readers and parameter ranges, which the views draw and the knobs span),
tw/plugins, tw/events (declared with the slice, proposal 56 §4.4). App edges:
actions, model, objects/sampler, objects/track, pluginui, shell (per
`tools/check_layering.py`). It sits at the RANK of `pluginui` and `eventui`.

* `pluginui` because it REGISTERS into pluginui's app-editor registry
  (`spluginappeditors.h`); pluginui never names this module.
* `shell` for `SApplication` (submit, the MIDI input hub) and
  `ssamplerwiring` (`slotHostsNassauDigital`, `programOf`,
  `newInstanceView`): the uid test, the program lookup and the per-user
  default view each have ONE reader, and it is the shell's.
* Not `servicesui`: the default view's `SOpt` key is read by the shell. The
  testkit reaches this module (`testkit -> samplerui`) for its three verbs.

Files: `ssamplereditor.h` (the window, its registry, the tap, the seams),
`ssamplerviews.h` (the four views), `ssamplerxmlpane.h` (the pane),
`ssamplerzones.h` (what the views draw, as pure functions over the program).

## Invariants

1. **ONE WINDOW PER INSTANCE, NOT A DOCK, OPENED FROM THE FX STRIP.** The
   window registers into pluginui's app-editor registry by the NassauDigital
   uid from a static initializer; the strip's Edit (button, double-click, row
   menu) consults the registry first (pluginui CONTRACT, "The app-editor
   registry"). The window is keyed by SLOT in a module-level registry and is
   owned by the strip's `window()` (the main window), never by the strip,
   which is deleted on every track switch. It is a top-level `Qt::Window`.

2. **NO CACHED ADDRESS.** The window holds the TRACK and the SLOT
   (`QPointer`s). Every submit derives `trackPath` — qualified by the root that
   holds the track, `splacements::specOf` — and `slotIndex` afresh; both are
   positions that move under a window that outlives the strip (the native
   editor's rule). An empty answer (the track left the project) refuses the
   submit with a reason. The slot going away closes the window
   (`SPluginSlot::slotDestroying`).

3. **THE `editorOpen` FLAG IS KEPT HERE**, by the native editor's rule: set
   when the window opens (before any show, so a headless open is right),
   cleared in `closeEvent`. A project saved with the editor open says so, and
   `restoreOpenEditors()` reopens it (not under `--test-case`).

4. **CLOSED MEANS DEAD, BEFORE THE DEFERRED DELETE.** `WA_DeleteOnClose`
   deletes through `deleteLater`, which a `--test-case` run may never deliver.
   `closeEvent` therefore leaves the registry at once, disconnects every
   project, program and application signal, releases the tap and sets
   `closed_`, which `refresh()` and `syncTap()` honour. Measured: without it a
   closed window re-acquired its tap on the next `arrangementChanged` and kept
   selecting zones from the device input.

5. **FOUR VIEWS AT FIXED SIZES** (DESIGN §7): Key 896×144, Pad 392×432, Map
   808×416, Zone 760×400, each `setFixedSize` in its constructor. The stack is
   pinned to the CURRENT page's size (a `QStackedWidget` otherwise takes its
   largest page) and the window's layout is `SetFixedSize`, so the window is
   exactly as large as the view shown plus the pane. Drawn in the application
   palette (the app theme sets it); the velocity gap in `BrightText`.

6. **THE VIEW IS THE INSTANCE'S, AND A SWITCH IS `set-sampler-view`.** The
   view buttons submit the verb (undoable, saved with the instance); the
   window follows the program's `viewChanged`, so an undo or a script moves
   it too. An instance with no program object yet shows the per-user default
   (`ssamplerwiring::newInstanceView`, `SOpt::SamplerDefaultView`, default
   `key`); the resolver gives a program object it CREATES the same view.

7. **KEY, PAD AND MAP DISPLAY AND SELECT ONLY** (the author, 2026-11-03).
   A press on the Key view selects the zones covering the key, on the Map view
   the zones whose rectangle holds (key, velocity): one `select-zones
   mode="replace"` each, undoable (the scripted verb's rule). A pad press is
   `set-sampler-view padBank pad`; a bank button the same, keeping the pad when
   it exists in the new bank and falling back to pad 0 when it does not (bank
   F's pads 12..15 have no key without a `<Pads>` row). Nothing else in these
   views writes. Dragging zone edges in the Map view is QBX-280.

8. **WHAT A VIEW DRAWS IS `ssamplerzones`, AND SO IS WHAT `describe()`
   REPORTS.** Every value comes from `twSamplerProgramModel::value()` (the
   core's text, parsed with the C locale). THE VELOCITY GAP (DESIGN §2): the
   velocities of `[zone.velLo, zone.velHi]` outside every layer's window, the
   window clamped into the zone's; a zone with no layer is one gap. The Map
   view hatches each gap across its zone's keys; it never invents a fill.

9. **THE ZONE VIEW'S KNOBS ARE THE EDITING SURFACE.** Rows Amp (level, pan,
   tune), Drive (drive, driveMix), Filter (cutoff, resonance,
   filterEnvAmount, filterKeyTrack), Envelope (attack, hold, decay, sustain,
   release), Layers (level, tune, pan of layers 1..4). A knob is a `QDial` of
   1000 steps (an Int with a smaller range: one step per value) over the
   parameter's range in the CORE'S table (`twSamplerProgramModel::paramRange`;
   cutoff on a log scale), so a value lands on the nearest step. The view shows
   the FIRST selected zone; a zone knob submits `set-zone zones="<every
   selected zone>"` (DESIGN §2.1's "the selected zones"), a layer knob
   `set-layer layer="<that layer of the shown zone>"`, always ABSOLUTE (`=`
   prefix). A knob with nothing to edit (no zone selected, a layer the zone
   lacks) is disabled.

10. **ONE KNOB DRAG IS ONE UNDO STEP.** The dial's `sliderPressed` mints a
    gesture token (`SAction::newGesture`), `sliderReleased` drops it, and every
    action of the drag is stamped with it; `set-zone` / `set-layer` merge on
    it (objects/sampler `SSetSamplerValuesAction::mergeKey`/`mergeWith`). A move
    with no press (a wheel notch, a key step) carries no token and is a step of
    its own (QBX-206's default).

11. **HAND SELECTION IS THE WINDOW'S OWN TAP, AND NOT AN UNDO STEP** (A12;
    the author's answers of 2026-11-03). While the window is open and the
    track's input is MIDI, it holds ONE sink of that port's `MidiInFanout`
    (`SMidiInputHub::fanoutFor`), with the input's channel mask: a second
    private ring, so the engine's live source and the shell's key taps keep
    theirs. It follows the track's input (`arrangementChanged`,
    `liveLanesMayHaveChanged`) and is released on close; a re-acquired sink is
    empty, so a note played while the window was closed selects nothing. A
    10 ms poll drains it: a note-on with velocity > 0 starts a new chord when
    no key was held, else joins it; a note-off or a velocity-0 note-on releases.
    The selection is every zone covering a key of the chord
    (`zonesCoveringKey`, the core's), each once, in the order the keys came, set
    DIRECTLY with `SSamplerProgram::setSelection` — not `select-zones`, so no
    undo step — and it stays after the keys are released, until another key
    is held. A chord whose keys cover no zone leaves an empty selection. Not
    saved (objects/sampler's rule for the selection).

12. **THE XML PANE APPLIES ONE VERB AS ONE SUBMIT.** The text must be ONE
    element (two are a parse error, and refused); the verb must be one of the
    instrument's (`SSamplerXmlPane::verbs()`: the twelve forward verbs, not the
    inverse forms); every attribute must be one the verb declares
    (`knownAttributes()`); `trackPath` and `slotIndex` are filled in when the
    element leaves them out (DESIGN §9.3) and taken as written when it does
    not. The action is built through the registry and `readXml` (the same path
    a `.qxa` takes) and submitted once, so one undo step; a refusal — by the
    pane or by the verb (the history's rejection count moved) — says why in
    the status line and adds no step.

13. **COMPLETION IS ONE ROUTINE OVER THE DECLARED ATTRIBUTES.**
    `completionsAt(text, cursor)`: right after `<`, the instrument's verbs;
    inside a tag after a blank, outside a quoted value, the verb's
    `knownAttributes()` minus `trackPath`, `slotIndex`, `gesture` and what the
    tag already has, filtered by the name being typed. Typing fills the
    completer's model from it (the popup shows only on a visible pane);
    accepting replaces the typed prefix — a verb followed by a blank, an
    attribute followed by `=""` with the cursor between the quotes.

14. **A DROP RESOLVES ITS TARGET IN THE VIEW, THEN ONE SUBMIT.** A view takes
    `application/x-smaragd-resource` and hit-tests the position itself: a key
    (Key view, empty Map grid), a pad's key (Pad view), a zone (a Map
    rectangle, the Zone view's zone). The rule (`SSamplerEditor::dropOn`, the
    simplest reading of DESIGN §9.3, mirroring §7's pad landing): a zone's
    layer 1 takes the source (`set-layer src`), a zone with no layer gets one
    (`add-layer`); a key's ONE covering zone is that zone; no covering zone,
    a new one-key zone rooted on the key (`add-zone`); more than one, REFUSED
    with the zones named. `file:<path>` is the src as written; `asset:<name>`
    links the asset, which only `set-layer` can do, so a new zone or layer for
    an asset is ONE `SCompositeAction` (the add with an explicit id from
    `nextId`, QBX-265, then `set-layer` on the layer id after it). `media:` is
    refused (the media cache's fetch hook is not in reach). Every landing drop
    is one undo step.

15. **NO PRODUCT NAME IN USER-VISIBLE TEXT** (`check_product_name`). The
    window title is the track's name and the instrument's descriptor name.

## Seams (off screen, the arranger-menu precedent)

No headless case can look at a widget, so the window has a DESCRIBE seam and
TRIGGER seams over the real widgets, and nothing is ever asserted through
geometry or a grab (sizes are asserted as constraints, minimum == maximum).

* `describe()` — one line of `|`-separated fields, APPENDED to and never
  inserted into: `open view page pageSize pageFixed sizes fixedAll program
  bank pad padKeys selected held tap zones gaps zoneShown knobs completions
  xml xmlStatus undoIndex title status`.
* `clickView` / `clickBank` (a real `QPushButton::click`), `clickPad` /
  `clickKey` / `clickMap` (a real mouse press at the target's position in the
  view SHOWN — a hidden view refuses, as a user cannot click it),
  `knobPress` / `knobSet` / `knobRelease` (`setSliderDown`, Qt's own spelling
  of a press, so the production slots mint and drop the token), `xmlType`
  (text and cursor, then the typing's own handler), `xmlAcceptCompletion`
  (the completer's own `activated`), `xmlApply` (the Apply button),
  `dropResource` (a real `QDragEnterEvent` + `QDropEvent`), `pollTap`.
* The testkit verbs: `sampler-editor` (trigger), `assert-sampler-editor`
  (describe, the registry, the flag), `sampler-hold-keys` (injects into the
  capture MIDI input, then `pollTap`).

## Gates

| Case | Claim |
|---|---|
| `sampler_q7_views` | Q7.1: the strip's Edit opens this window and no slider editor; the four views at their fixed sizes; a switch is one undo step and the window follows undo, redo and a script; the view and the flag survive a save and reload; a close clears the flag |
| `sampler_q7_default_view` | Q7.1: a new instance starts in the per-user default (editor without a program; the program a verb creates, by `add-zone` and by a pad-only `set-sampler-view`); an existing instance keeps its view; an unknown value reads as Key; saved views survive a reload whatever the setting says |
| `sampler_q7_hand_select` | Q7.2: one key selects its zone, two keys select both (each zone once, in key order), a chord grows while a key is held, the selection stays after release, another key replaces it, velocity 0 releases, a key with no zone empties it; no undo step (one undo takes back the view switch made before every key); the tap follows the input and its channel, exists only while the window is open, starts empty; a sequenced note selects nothing; not saved |
| `sampler_q7_xml_pane` | Q7.3: one apply is one undo step (`undoIndex`), the address implied (on a second instance on track 1: the verbs' own `trackPath` default is "0", so track 0 cannot show it) or written; five refusals add no step and say why; verb and attribute completion, the address and written attributes left out, nothing inside a value or outside a tag, the cursor deciding |
| `sampler_q7_knobs` | one drag one step, two drags two, no press one step per move; layer knobs; several selected zones in one step; the verbs' gesture merge without the widget, and what never merges (relative, other targets, other parameters) |
| `sampler_q7_select_views` | what the views are handed (zones, gaps with clamping, a layerless zone, a closed gap, pad keys with a reassigned pad); Key and Map presses select (undoable, editing nothing); pad and bank presses; bank F's keyless pads |
| `sampler_q7_drop` | a file on an empty key, on a covered key, on a key two zones cover (refused, unchanged), on a Map rectangle and on empty Map grid, an asset as one composite (one undo), on a pad, on the Zone view's zone with and without a layer; media and unknown payloads and a hidden view refused |

### Sabotages (QBX-279)

38, each watched failing (the harness restores every file from its own saved
bytes and touches it); 37 bite. The one that cannot: an asset drop on an empty
key without the explicit zone id — `add-zone` takes `nextId` anyway, the same
id the composite's `set-layer` names, so the id only pins what already holds.
Two needed the cases sharpened first: the implied address (the instance moved
to track 1, see above) and completion inside a quoted value (a value holding a
blank). The full list is in proposal 56 §11 "Q7 as built".

## Not gated / known limits

- **Q7.4** (every label and value readable in every view; a velocity gap
  visible in the Map view) is the AUTHOR'S hand check. The cases gate that the
  view is HANDED the gaps (`gaps=`), not that a person can see them.
- Nothing is painted in a gate: `paintEvent` runs on no headless path, so a
  view that drew nothing would pass every case. The popup of the completer is
  never shown either (it shows only on a visible pane).
- `restoreOpenEditors()` reopening the window after a load returns at once
  under `--test-case`; the flag's round trip is gated, the reopen is not.
- A `media:` drop is refused, not placed; an OS file drop (`text/uri-list`)
  is not taken. A drop's key on the Pad view is the pad's key as the program
  maps it; a pad with no key takes no drop.
- The knob step (1000 per range, linear except cutoff) is coarse for wide
  ranges such as the envelope times (`kMaxTime`); a value typed in the XML
  pane is exact.
- Clicking a zone in the Key or Map view is an undo step (`select-zones`)
  while a held key is not; that split follows the author's answer for held
  keys and Q1's verb for the rest.
