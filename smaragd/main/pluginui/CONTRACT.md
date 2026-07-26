# app/pluginui — CONTRACT

Purpose: plugin UI — browser dialog (lists the tw/plugins registry), effect
strip, parameter editor, plugin slot widget.

Public headers: app/pluginui/*.h

Depends on (engine): tw/plugins (+core/graph). App edges: model,
objects/mixer, objects/track, shell.

Invariants:
1. The browser reflects pluginRegistry().rescan() — no separate plugin list.
2. Parameter edits go through the plugin's setParam on the UI thread; the
   audio thread reads params lock-free inside the plugin.
3. Insert/remove plugin mutations are ACTIONS (objects/mixer), not direct
   chain pokes from dialogs.
4. A slot's state is READ from the model, never inferred from the widget
   (proposal 08 M4 supplies it): SPluginSlot::getSlotState() is Active /
   Missing / Unsupported, getSlotMode() is the channel mapping, and
   getDescriptor() is what the project file said (getEffectiveDescriptor() is
   what the registry resolved). A Missing slot is transparent and its stored
   state chunk (getSavedState()) is untouched — a UI that "helpfully" wrote a
   fresh chunk for it would destroy the user's patch.
5. Reloading a previously-missing plugin is SPluginSlot::reloadPlugin() (UI
   thread; it reaches twPlugin::prepare()). It re-resolves the descriptor
   against the registry, re-instantiates in place — the DSP graph is not
   rebuilt — re-applies the stored state chunk, stales the pages and emits
   pluginReloaded(). Any editor showing that slot's parameters must re-read
   them on that signal.

How to test: manual; render_sawtooth_with_effects.qxa covers the hosted
chain audibly.

Known debt: no per-plugin editor plugins (generic parameter editor only). The
per-slot Reload affordance for a Missing plugin (and the greyed rendering of
Missing/Unsupported slots) is M5's; the model side is in place.
