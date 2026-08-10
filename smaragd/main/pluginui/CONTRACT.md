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
6. An edit is inaudible unless the slot is told. Two caches sit in front of the
   plugin — twPluginSlotProcessor's all-bus page cache and each per-bus tap's
   frozen pages — so a parameter write must be followed by
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

How to test:
- `qxa.plugin_bypass_and_param` — a bypass toggle and a parameter edit each
  change the rendered level, and each undoes (invariants 3 and 6).
- `qxa.plugin_remove_restores_param` — undoing a removal restores the plugin's
  PARAMETERS, not just the slot.
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

Known debt: no per-plugin editor plugins — generic sliders only, on a fixed
1000-tick normalization, and no native `clap_plugin_gui` / `IPlugView`
embedding (a deliberate deferral of proposal 08). The drop handler's
`dragSourceIndex_` is never assigned by any drag START — `startDragFromPlugin()`
was declared and never defined — so drag-to-reorder cannot fire today even
though `reorder-plugin` behind it is tested; that is pre-existing and untouched
by M5.
