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

How to test: manual; render_sawtooth_with_effects.qxa covers the hosted
chain audibly.

Known debt: no per-plugin editor plugins (generic parameter editor only).
