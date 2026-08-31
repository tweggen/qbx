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

## The native editor window is never parented to the FX strip (fixed 2026-08-23)

`SPluginNativeEditor`'s own header comment already said the window must not be
parented to `SPluginEffectStrip` — `STrackDetailPanel::rebuildUI()` deletes the
strip on every track selection change (a lane click, a clip click, or an
explicit track switch), which would take the window down as a Qt child — but
the implementation contradicted it: the constructor did `QDialog( parent )`
with `parentForPosition` passed straight through as the real Qt OWNERSHIP
parent, and `SPluginEffectStrip::openParamEditor()` passes THE STRIP ITSELF.
`SPluginNativeEditor::restoreOpenEditors()` passes `SMainWindow` instead, which
is why a RESTORED editor never showed the bug and only an Edit-button-opened
one did — that asymmetry is what gave the root cause away.

**The fix**: the constructor now climbs to `parentForPosition->window()` — the
durable TOP-LEVEL ancestor — before handing it to `QDialog`. A dock, a strip, a
panel: whatever transient widget a caller has on hand, its `window()` is the
long-lived `QMainWindow`, which is never deleted while the app runs.
`parentForPosition->window()` on `SMainWindow` itself is `SMainWindow`
(`window()` on a window is itself), and on `nullptr` is `nullptr` — both
existing callers (`restoreOpenEditors`, the testkit's headless open) are
unaffected. `parentForPosition`'s only remaining job is that one climb; it is
not read for anything else — `attachPlugin()`'s D1 floating-rung transient
parent already reads `parentWidget()->window()`, which now equals what the
constructor computed.

**A window that survives a strip rebuild also fixes the "will not reopen"
report**, though this could only be verified by removing the abrupt teardown
rather than proven by isolating the original failure: the FX strip being
deleted destroyed the window as a Qt child WITHOUT running `closeEvent()`
(a plain `delete`, not `close()`), so geometry was never saved and
`slot_->setEditorOpen(false)` never ran — every subsequent Edit press then
depended on the plugin backend surviving an unclean teardown to attach a second
time, which is exactly the kind of thing a real plugin SDK is not guaranteed to
tolerate. With the window never destroyed by a rebuild, there is nothing left
to fail to recreate.

**No transport guard exists and none should be added.** The only `isPlaying()`
read in this module is the automation-recorder routing inside `applyEdit()`
(`SApplication::app().isPlaying()`), which decides whether a knob move is
captured as a Touch/Latch/Write point — it never gates whether `openFor()`
opens a window.

Gated: `qxa.plugin_native_editor_survives_strip` (new), which drives the exact
FX-strip-parented open through the testkit's new `plugin-native-editor
action="open-via-strip"` mode — build a throwaway `SPluginEffectStrip`, open
through it (exactly `openParamEditor()`'s own call), then destroy the strip
(exactly `rebuildUI()`'s `delete pluginStrip_`) — and asserts the editor is
STILL open. Watched failing on the pre-fix binary: `expectOpen="1"` read
`isOpenFor()==false`. **NOT gated, hand-verify only:** whether a REAL plugin's
window visually stays anchored above the main window after the reparent (no
display in a `--test-case` run); the reopen-after-abrupt-teardown failure mode
itself, since it needs a real plugin GUI and is removed by construction rather
than independently reproduced; raising a FLOATING (CLAP D1) editor on a second
Edit press — the ABI (`twPluginEditor`) has no "bring to front" call for a
window the plugin itself owns, so a floating editor's Edit press is a no-op by
design, unchanged by this fix, and is a genuine gap rather than something this
fix could close.

## Follow-up (2026-08-23, same day): the generic editor had the IDENTICAL bug, and "will not reopen" was never the native editor at all

Two more things were found chasing the "does not reopen" report further, both
from a coordinator's review that (correctly) pushed back on the previous
section's "removed by construction rather than independently reproduced"
claim — that claim was too weak, and the real mechanism turned out to be
different from the one guessed at above.

**1. `SPluginEffectStrip::ensureParamEditor()` had the SAME parenting bug as
`SPluginNativeEditor`**, and on a platform where a plugin's native editor is
refused (Linux/X11 VST3, `caps().needsRunLoop`), the generic slider list is
the window the user actually sees — so fixing only the native path left a
live field bug for exactly that case. It did `dlg = new QDialog(this)` (the
STRIP) and kept the dialog in `editors_`, a STRIP MEMBER — so
`STrackDetailPanel::rebuildUI()`'s `delete pluginStrip_` destroyed this
editor too, on every track switch, for the identical reason.

**Fixed the same way**: the dialog now parents to `window()` (the strip's own
durable top-level ancestor, computed via the SAME QWidget method the native
editor's constructor climbs to), and the registry moved OFF the strip into a
MODULE-LEVEL map keyed by `SPluginSlot*` (`genericEditorRegistry()` in
`splugineffectstrip.cpp`), mirroring `SPluginNativeEditor::registry()` —
critically, NOT just the parenting: reparenting the dialog while leaving the
registry on the dying strip would have made the map die with the strip while
the dialog survived as an orphan, and the NEXT strip would create a
DUPLICATE editor for the same slot. `SPluginEffectStrip::isGenericEditorOpenFor()`
/ `closeGenericEditorFor()` are new statics giving this path the same
`isOpenFor()`/`closeFor()` symmetry the native editor already had — needed by
the testkit (`plugin-generic-editor`) and available to any future caller.

Gated: `qxa.plugin_generic_editor_survives_strip`, over `tw.test.clap.
stereoskew` (no `clap.gui`, so `openParamEditor()` genuinely falls through to
`ensureParamEditor()` rather than merely failing a native attach) — a
throwaway strip parented to the REAL main window (the same caveat as the
native gate's own case: a parentless strip's `window()` is itself, which
would silently retest the pre-fix shape), opened twice, then closed.

**2. A SEPARATE, PRE-EXISTING crash, unrelated to either parenting bug**, was
found while testing the "why does a second open fail" hypothesis directly
with gdb rather than assuming: open a native editor, never close it, let the
project tear down. `SPluginNativeEditor` closed its window by connecting to
`QObject::destroyed()` on the slot — a signal that fires from `~QObject()`,
the BASE class, which runs strictly AFTER `~SPluginSlot()`'s own body and
every member (`proc_`, the live plugin instance) has already been destroyed.
`close() -> closeEvent() -> twPluginEditor::detach()` therefore touched an
ALREADY-FREED plugin — SIGSEGV inside `twClapEditor::detach()`.

Fixed by `main/objects/track/CONTRACT.md`'s new `SPluginSlot::slotDestroying()`
signal, emitted first thing in a no-longer-`=default` `~SPluginSlot()`, while
the plugin is still alive; both this class and the generic editor's
`ensureParamEditor()` now connect to it instead of `destroyed()`.

**A THIRD, downstream crash was uncovered fixing the second**: correctly
closing an editor during project teardown POSTS a deferred delete
(`QDialog::close()` on a `WA_DeleteOnClose` widget), and a `--test-case` run
leaves through `std::exit()` with no event-loop pump in between — so that
deferred `~QWidget()`/`~QWindow()` ran from a Qt-internal atexit hook AFTER
the platform integration had already torn down. SIGSEGV inside
`QWindow::~QWindow()`, downstream of `QOpenGLContext`. Fixed by draining
`QEvent::DeferredDelete` in `smaragdOrderlyShutdown()` (`main/shell/src/
main.cpp`), before `std::exit()`, while `QApplication` is still fully alive —
see `main/shell/CONTRACT.md`.

**The generic editor's own connection was ALSO switched to `slotDestroying()`,
but for consistency, not a proven crash**: `SPluginParamEditor` holds no ABI
`twPluginEditor` to `detach()`, and a dedicated probe (open it, never close
it, let the project tear down) found no repro under the old `destroyed()`-based
connection either way. Recorded rather than silently assumed safe — it still
reads `slot->getProcessor()` in a few of its own slots
(`onParamsChanged()`, `onMeterTick()`), and nothing guarantees none of those
can fire in the gap `destroyed()` leaves open.

Gate: `qxa.plugin_native_editor_teardown_safe` — deliberately violates
`qxa.plugin_native_editor`'s own stated discipline ("a case that opens one
MUST close it"): a forgetful caller, or simply closing a project with an
editor still open (the ordinary shape for a real user), must not crash the
app. There is no explicit "did not segfault" assertion; CTest catches that by
EXIT CODE, the same mechanism that already catches `qxa.split_plain_
screenshot`'s teardown crash (CLAUDE.md, "Two known crash flakes"). Watched
segfaulting, reliably, under BOTH sabotages independently (reverting
`slotDestroying()`'s connection alone; restoring that but reverting the
`DeferredDelete` drain alone) — each crashes at a DIFFERENT point in the
stack, confirming they are two distinct fixes, not one. A two-plugin variant
of the same script (adding a second track/slot for the generic editor) was
tried FIRST and did not reproduce reliably: a use-after-free's crash depends
on allocator state, so it can silently read stale-but-harmless memory once
the heap shape changes underneath it. The committed gate is deliberately the
smallest script that reproduces every time.

### A parameter slider resets to the PLUGIN'S declared default on double-click

`SPluginParamEditor` wires every slider through `sdefaultreset::onDoubleClick`
(`app/model/sdefaultreset.h`, shared with the two detail panes — see
`main/timeline/CONTRACT.md` inv. 46) to `info.defaultValue`, the value the
plugin itself reports.

It restores through `setParamFromUi()`, never `slider->setValue()`. A
parameter already at its default quantises to the SAME tick it currently
holds, `setValue()` then emits nothing, and the reset would silently do
nothing on a control the user had moved between two values sharing one tick.
`setParamFromUi()` exists precisely to run the handler in that case, so the
reset always travels the production path: the quantisation, the automation
write-pass offer, and the `set-plugin-param` action that broadcasts to every
instance and stales the slot's pages.

Gate: `qxa.detail_pane_reset_defaults` section 5, through
`plugin-editor-set-param gesture="double-click"` — watched failing with the
wiring removed (the label still read the moved value, "250", where the
plugin's default is "100").

### The FX strip has no scroll area and no minimum height of its own

Both were removed in the same fix as `main/timeline/CONTRACT.md` inv. 45, and
for the reasons stated there: an explicit `minimumHeight` REPLACES the
layout-derived one (so a 100 px floor over a row list plus a button row laid
the button row on top of the list), and the panel above now scrolls as ONE
surface. The strip reports what its rows actually need and lets that panel's
scroll area absorb the rest. A strip that reintroduces either will crush again
at exactly the sizes `qxa.track_detail_layout` measures.
