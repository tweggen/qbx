# app/pluginui — CONTRACT

Purpose: plugin UI — browser dialog (lists the tw/plugins registry), effect
strip, parameter editor, plugin slot widget.

Public headers: app/pluginui/*.h

Depends on (engine): tw/plugins (+core/graph). App edges: model,
objects/mixer, objects/track, shell.

Invariants:
1. The browser reflects pluginRegistry().rescan() — no separate plugin list.
2. Parameter edits reach the plugin on the UI thread; the audio thread reads
   params lock-free inside the plugin. They reach it THROUGH an action (see 3),
   never by calling twPlugin::setParam() from a widget.
3. Every model mutation from this module is an ACTION, never a direct chain or
   slot poke from a dialog. Since proposal 08 M5 that is all five of them:
   `insert-plugin`, `remove-plugin`, `set-plugin-bypass`, `reorder-plugin`,
   `set-plugin-param` (objects/track). The two exceptions are deliberate and
   are not document mutations: `SPluginSlot::reloadPlugin()` (see 5) and
   opening/closing an editor window.
4. A slot's state is READ from the model, never inferred from the widget
   (proposal 08 M4 supplies it): SPluginSlot::getSlotState() is Active /
   Missing / Unsupported, getSlotMode() is the channel mapping, and
   getDescriptor() is what the project file said (getEffectiveDescriptor() is
   what the registry resolved). A Missing slot is transparent and its stored
   state chunk (getSavedState()) is untouched — a UI that "helpfully" wrote a
   fresh chunk for it would destroy the user's patch. The greyed row therefore
   shows getDescriptor()'s name, NOT the placeholder's, because the point of
   the row is to say which plugin is missing.
5. Reloading a previously-missing plugin is SPluginSlot::reloadPlugin() (UI
   thread; it reaches twPlugin::prepare()). It re-resolves the descriptor
   against the registry, re-instantiates in place — the DSP graph is not
   rebuilt — re-applies the stored state chunk, stales the pages and emits
   pluginReloaded(). Any editor showing that slot's parameters must re-read
   them on that signal. It is NOT an action: it mutates no document state (a
   re-save produces the same bytes), so there is nothing to undo.
6. An edit is inaudible unless the slot is told. A cache sits in front of the
   plugin — the slot's single twPluginInsert's frozen pages (before proposal 36
   B4 there were two, because the processor also kept an all-bus page cache for
   its sibling taps) — so a parameter write must be followed by
   SPluginSlot::notifyPluginEdited(), and a bypass must go through
   SPluginSlot::setBypass(). Both then invalidate UPWARDS by emitting
   SPluginSlot::audioInvalidated(), which the owning STrack turns into
   invalidateRenderPath(). The slot cannot do that itself: an SPluginChain is
   deliberately not an SLink child of its track, so
   SObject::invalidateRenderPath()'s root-down walk never reaches a slot. That
   was a real defect — after M3, bypass and parameter edits still rendered
   byte-identical audio (proposal 08 M5 fixed it and gates it in
   plugin_bypass_and_param.qxa).
7. An editor must survive its slot dying and its slot MOVING. SPluginEffectStrip
   keys its editor windows by slot and closes one on the slot's destroyed()
   signal (remove-plugin deletes the slot), and re-points every open editor at
   the slot's new index on each rebuildUI() — reorder-plugin moves a slot without
   touching the dialog, and an editor holding the old index would aim its next
   parameter edit at a DIFFERENT plugin. A per-row signal connection whose
   context is the strip would also pile up one per rebuild; use the row's
   container widget, which dies with the rebuild.
8. The strip addresses its track by INDEX-PATH from the root mixer
   (trackPathString() -> strackpath::pathOf), never by scanning the mixer's
   direct children. Every button handler early-returns when that string is
   empty, so a wrong answer disables the ENTIRE strip silently — buttons
   enabled, clicks accepted, no action ever submitted — rather than misbehaving
   visibly. It did exactly that for any track nested in a folder, which is what
   "I cannot remove a plugin even if it is missing" turned out to be: the
   missing-ness was incidental (remove-plugin is Missing-tolerant and the Remove
   button is enabled in every state), the nesting was the bug. Because the
   failure is invisible from outside, describeSlot() reports the resolved
   `trackPath=` and `assert-plugin-strip` takes a `trackPath` attribute;
   `qxa.plugin_strip_nested_track` is the gate.

9. **A slider write during a Touch/Latch/Write pass goes to the RECORDER, not
   to `set-plugin-param`** (proposal 37 P6). `onParamSliderChanged` offers the
   value to `SApplication::automationRecorder()` first and returns if it was
   taken; the whole pass then lands as ONE `set-automation-points` when the
   control is released or the transport stops. Its release is also a PUNCH-IN
   signal — and **since proposal 33 M5 it is no longer the only one.** A native
   plugin editor raises the other: `SPluginNativeEditor::applyEdit` offers every
   value the plugin's own GUI reports to the same recorder, first, exactly as
   the slider does. (What is still unreachable is the gesture stream inside
   `process()`: `ParamGestureBegin/End` arrives on a worker thread at freeze
   time and nothing consumes it. The editor's gestures come from
   `IComponentHandler` on the MAIN thread instead, which is why they are usable
   and those are not.)

   **A native editor's phases are HINTS, not undo boundaries** (proposal 33
   §4.1, measured): Dexed brackets every single value in its own
   `beginEdit`/`performEdit`/`endEdit`, so one drag is dozens of gestures. One
   undo entry per gesture would be one per mouse step. Coalescing is by
   `(track, slot, paramId)` through `SSetPluginParamAction::mergeWith()`, and
   the poll batch is coalesced per parameter before anything is submitted
   (§4.2 — a batch can hold several moves of one knob).
   While a Read-family `param:` lane exists the slider DISPLAYS the curve at
   the position being heard (pumped from `SApplication::meterTick`), except on
   a control that is being recorded, which must show the hand and not the
   curve.


How to test:
- `qxa.plugin_bypass_and_param` — a bypass toggle and a parameter edit each
  change the rendered level, and each undoes (invariants 3 and 6).
- `qxa.plugin_remove_restores_param` — undoing a removal restores the plugin's
  PARAMETERS, not just the slot.
- `qxa.plugin_editor_persistence` — D2's project half: the flag starts false,
  opening sets it, it survives save+load, closing clears it, and the real
  post-load walk opens NOTHING in a test-case run (watched failing with the
  guard removed). Also asserts that a headless run leaves no
  `editorGeometry` key in the shared `smaragd.ini`.
- `plugin_editor_geometry_test` — D2's SSettings half: the off-screen clamp,
  which cannot be reproduced by hand without unplugging a monitor.
- `qxa.plugin_native_editor` — the native editor END TO END with no display:
  the "native" answer from `editorKindFor()`, the plugin's own knob reaching the
  render (0.0667 -> 0.1666, exactly 2.5x, set by nobody in the script), ONE undo
  entry that actually restores the level, and the window closing before the
  plugin is torn down. Watched failing before the `previousValue` fix.
- `qxa.plugin_ui_strip_and_editor` — the two testkit verbs
  `assert-plugin-strip` / `plugin-editor-set-param` build the real
  SPluginEffectStrip and SPluginParamEditor off screen (never shown: a qxa run
  on Windows uses the real platform plugin) and assert the Active row, the
  greyed Missing row with its reason tooltip and Reload, the editor→action
  wiring, and reorder-plugin.
- `qxa.plugin_strip_nested_track` — the strip resolves a NESTED lane's path, and
  insert/remove on it work (invariant 8). Negative-control proven: restoring the
  top-level-only scan makes it fail with `trackPath=` empty.
- `qxa.plugin_slot_roundtrip` — extended with a bypass on a LOADED slot, which is
  the only gate on the `STrack::adoptPluginChain()` half of invariant 6's wiring
  (a loaded chain's slots never emit `slotInserted` at anyone).
- `qxa.render_sawtooth_with_effects` covers the hosted chain audibly.
- Still manual: that the strip and the editor LOOK right, the double-click
  gesture, the drag-to-reorder gesture, and the browser dialog.

Value display: `SPluginParamEditor::formatValue()` prefers the live plugin's own
value-to-text — `livePlugin()->paramValueText(id, v)` — so a value shows in the
plugin's units / enum names (e.g. "-6.0 dB", "440 Hz", "Sine"), and falls back to
the numeric rendering (int if `isStepped`, else 3 decimals) only when the plugin
returns empty. `valueLabelText(row)` (a headless seam) reads back what a row's
label displays; `SPluginEffectStrip::editorValueText()` exposes it, and
`plugin-editor-set-param`'s `expectValueText` asserts it in
`qxa.plugin_ui_strip_and_editor`.

Instruments in the UI (proposal 37 P3b, design 6.1). Everything the user sees
about an instrument is DERIVED from `slot->getDescriptor().isInstrument` and
`STrack::instrumentSlot()`; nothing about the role is stored twice.
- The browser has a KIND filter (`SPluginBrowserDialog::Kind` All / Instruments
  / Effects). It NARROWS the one list rather than being a second dialog — an
  instrument and an effect are the same kind of object with a different role —
  and the user can always widen it back to All, because refusing to SHOW a
  plugin is not the same as refusing to insert it (the insert rules live in the
  action, `objects/track/CONTRACT.md` inv. 11).
- The FX strip has "+ Add Instrument" beside "+ Add Effect". It opens the
  browser on Instruments and submits the SAME `insert-plugin` action, which is
  what puts the slot at index 0 and refuses a second one; the button HIDES
  itself once the track has an instrument, because an affordance that can only
  fail is worse than none (`addInstrumentEnabled()`).
- The instrument's row is row 0 because it IS slot 0. It carries an ACCENT
  BORDER (never a background fill — no row sets one, because a colour written
  here is a colour the theme cannot reach, and the near-white fill this row
  used to carry was unreadable under the shipped dark theme's `#E6E0DD` ink),
  is prefixed with an eighth note, carries its own tooltip line, and is NOT
  draggable — a reorder across slot 0 is refused, so offering the grab would
  only produce a rejection. `describeSlot()` reports `kind=instrument|effect` between `mode=`
  and `bypass=` (see ACTIONS.md for why not next to `name=`).
- The arranger head's "I" button was already derived, by P4
  (`SSMVMixerControl::hasInstrumentSlot`); P3b made it non-empty for the first
  time and gated it (`assert-track-head contains="I=1"` in
  `qxa.instrument_slot_rules`).

Native editors: a **VST3 or CLAP** plugin's OWN GUI opens embedded in
`SPluginNativeEditor` (proposal 33 M4/M6), and the generic slider list is the
FALLBACK, not an alternative — it is what opens when there is no native editor,
when `createEditor()` returns null, when `attach()` is refused, or when the
plugin needs an X11 run loop we do not provide. There is deliberately no second
entry point to the sliders while a native GUI exists (decision D3), which is
why every native edit has to be undoable.

**THE PRE-EDIT VALUE TRAVELS WITH THE EDIT, and skipping it makes undo a
silent no-op.** `twPluginEditor::poll()` applies each Change to the mirror and
the DSP on its way through — that is what makes a plugin's own knob audible
whatever the host does — so by the time `applyEdit()` runs, `getParam()` already
returns the NEW value. `SSetPluginParamAction::apply()`'s ordinary baseline is
exactly that call, so its inverse restored the value it had just set. Shipped in
M5, caught by `qxa.plugin_native_editor` (the undo render read the GAINED level),
fixed by `twEditorParamEdit::previousValue` plus
`SSetPluginParamAction::setPreviousValue()`. The native editor is the ONLY caller
of that setter and must stay so: every other edit reaches the plugin THROUGH the
action.

**A floating editor is a real outcome, not an error path** (decision D1, CLAP
only). When `attach()` refuses, `attachFloating()` is offered before the generic
fallback: the plugin owns a top-level window of its own and this `QDialog` is
never shown — it stays alive purely as the poll pump and the registry entry, so
`openFor()`, the already-open branch and `resizeToPlugin()` all check
`isFloating()`. The transient parent handed to `set_transient` is the
APPLICATION window, not this dialog: an invisible parent keeps nothing above
anything. VST3 and AU have no floating form and never take this rung.

**THE EDITOR'S OPEN STATE AND ITS GEOMETRY ARE PERSISTED IN DIFFERENT PLACES,
and that split is the design** (decision D2). WHETHER the editor was open goes
in the PROJECT (`<SPluginSlot editorOpen='true'>`), because "this project opens
with the synth's editor up" means the same thing on any machine. WHERE the
window was goes in `SSettings`, per user, keyed `<format>:<uid>`, because
monitor layout is machine-local and a window at x=2400 restored on a laptop is
a window nobody can reach. It is the same split, and the same reason, as
proposal 37 P7's portable `midiOutPort` NAME against the machine-local
`midi/portId/<name>`.

Four consequences a change must preserve:

- **`editorOpen` is written only when true**, so every project file saved before
  D2 — and both render goldens — stay byte-unchanged. Same discipline
  `SObject::serialize()` already follows for automation lanes.
- **It is NOT an action.** Opening a window is not an edit to the arrangement,
  and an undo stack that has to be walked past a window-open entry to reach a
  real edit is worse than a project flag that is not undoable.
- **A restored geometry is never trusted blindly.** `clampOntoAScreen()` is a
  pure static (hence gateable: `plugin_editor_geometry_test`) and keeps the SIZE
  while moving the position only as far as it takes to land on an available
  `QScreen`. The size is restored ONLY when `caps().resizable` — a fixed-size
  editor has one correct size, the one it just reported, and forcing a
  remembered one on it clips its own drawing silently.
- **`restoreOpenEditors()` does nothing in a `--test-case` run.** A qxa run uses
  the REAL platform plugin (the suite does not set `QT_QPA_PLATFORM=offscreen`),
  so an unguarded restore would put plugin windows on the developer's desktop
  mid-suite. A case asserts the flag through the MODEL, never through a window.
  It is called from the END of `SMainWindow::openProject()` — the one place that
  happens once per load and where every dock already exists — which is why
  `shell -> pluginui` is a declared edge in `tools/check_layering.py`.

**The window is NOT owned by the strip**, and must not become so:
`STrackDetailPanel::rebuildUI()` deletes the strip on every track switch, so a
window parented to it would vanish when the user clicks another lane. It lives
in a module-level registry keyed by `SPluginSlot*`. For the same reason it does
not cache `(trackPath, slotIndex)`: both are positions that move under a window
that outlives the strip, so they are DERIVED at every commit. Inv. 7's
re-pointing is the generic editor's answer to the same problem; a native window
has no rebuild to hang it on.

Known debt: generic sliders are still a fixed 1000-tick normalization;
**AudioUnit has no native editor yet** (proposal 33 M7) and neither does
Linux/X11 (M8 — VST3 there needs `IRunLoop`, which the app must bridge to
`QSocketNotifier`/`QTimer`; CLAP on X11 does not, so `caps().needsRunLoop` is
false in that backend on every platform). D2 persistence IS implemented (see
above); what it does NOT do is remember a per-SLOT geometry, only a per-plugin
one. **What is gated headlessly is the parameter FLOW, not the window**: `qxa.plugin_native_editor` drives a real `SPluginNativeEditor`
against `tw.test.clap.gui`, a fixture that implements `clap.gui` and creates no
window at all, with `showWindow = false` so nothing reaches the screen (a qxa
run uses the real platform plugin). Whether a real plugin's GUI actually draws
inside our container — and keyboard/focus routing, host-driven resize, and DPI
on a scaled monitor — remains hand-verification only. The drop handler's
`dragSourceIndex_` is never assigned by any drag START — `startDragFromPlugin()`
was declared and never defined — so drag-to-reorder cannot fire today even
though `reorder-plugin` behind it is tested; that is pre-existing and untouched
by M5.

## The plugin latency badge (proposal 21 L5)

The FX strip shows each slot's `SPluginSlot::reportedLatencyFrames()` in ms, and
the chain's total below the rows — both only when they are non-zero, so a chain
of ordinary effects looks exactly as it did.

Two invariants:

- **`describeSlot()` appends `|latency=<frames>` AT THE END.** The proposal-08 M5
  cases assert contiguous SPANS of that string (`name=…|state=…`,
  `nameEnabled=…|…|reload=…`), so a field inserted among them would break
  assertions that are about something else entirely. Gated by
  `plugin_ui_strip_and_editor` (`contains="latency=0"` — the test CLAP reports
  none, which is the honest expectation and still proves the number reaches the
  widget).
- **The number is REPORTED, never compensated, and every mount says so.** Plugin
  delay compensation is out of scope (proposal 37 P9), and the LIVE lane has no
  delay line anywhere: the pump renders block-wise straight into the ring. A
  latency-reporting plugin monitored live is heard late by exactly the badge's
  number, and the row tooltip, the chain footer's tooltip and the transport
  bar's latency readout all state it.

## A plugin restart is metadata, and only acted on when it changed something

`SPluginNativeEditor::handleRestart()` calls `SPluginSlot::notifyPluginEdited()`
only when the slot's `reportedLatencyFrames()` or its `paramRows()` count
differs from the one the window last saw. Identical means the restart carried
nothing, and it is logged at debug and dropped.

**Without this, playback does not start.** `twPluginSlotProcessor` resets a
plugin instance whenever a page arrives out of order; an iPlug2-built plugin
answers a reset by re-reporting its latency, and both SDK backends notify the
host UNCONDITIONALLY (`IPlugVST3::SetLatency` → `restartComponent`,
`IPlugCLAP::SetLatency` → `request_restart`), whether or not the number moved.
`notifyPluginEdited()` stales the whole render path above the slot, so the pages
are re-frozen out of order and the instance is reset again — the readahead never
reaches the three seconds the transport waits for. Measured with the guard
disabled: **about 25 restarts per second**, each one an invalidation.

Three things about the shape of it:

- **Comparing those two values compares everything a restart can tell us.** A
  parameter VALUE change does not arrive this way — VST3's `kParamValuesChanged`
  is filtered out in `twVst3Editor::poll()`, and CLAP values come as out-events.
  What is left is metadata: the latency badge and the parameter list.
- **The first restart after the window opens is always acted on** (both
  baselines start at −1, which no latency or count can be). The host has not
  read the plugin's configuration since the window existed, so it has nothing to
  compare against and must not guess.
- **Rate-limiting was considered and rejected.** A slow loop is still a loop.

The engine-side twin — a restart raised from inside a call the host itself made
is never reported at all — is `tw303a/plugins/CONTRACT.md` invariant 53. That
one is VST3-only by construction; this one is what catches CLAP, where iPlug2
defers the request to the main thread and it arrives long after its cause.

Gated by `qxa.plugin_restart_no_livelock` against the `tw.test.clap.restart`
fixture, which is `tw.test.clap.gui` plus that one habit. The case asserts the
MECHANISM (at most one restart acted on per second of playback, and the guard
seen declining the rest) and deliberately not the SYMPTOM: on a 4-second
sawtooth through one arithmetic-only plugin the freeze outruns the loop and the
playhead advances even with the guard disabled.
