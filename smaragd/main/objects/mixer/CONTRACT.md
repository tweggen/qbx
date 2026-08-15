# app/objects/mixer — CONTRACT

Purpose: the root container object (SStdMixer: ordered tracks, asset
management) and SPluginChain (per-track plugin hosting model), with asset
actions (create/place/remove-asset, remove-asset-placement) and plugin
actions (insert/remove-plugin).

Public headers: app/objects/mixer/*.h

Depends on (engine): tw/core, tw/graph, tw/mix, tw/plugins. App edges: per
tools/check_layering.py.

Invariants:
1. getNTracks() counts TOP-LEVEL children only (reparenting under a group
   reduces it — assertions in tests rely on this).
2. Assets are objects owned by the project and placed via links; removing
   the last placement does not delete the asset (remove-asset does).
3. SPluginChain mirrors its model into tw/plugins chain components; wiring
   rebuilds go through rebuildWiring() after input changes.
4. Track selection is a SET with one distinguished PRIMARY, and both are
   held as QPointers — a removed track survives on the undo stack but dies
   when that command is discarded, so raw pointers here would dangle until
   the next click. Every mutator funnels through setSelectedTracks(), which
   is the one place that normalizes (no nulls, no duplicates), decides the
   primary (a member, else the last entry), and emits: selectedTracksChanged()
   for the set, then selectedTrackChanged() for the primary. The primary is
   what "the selected track" means everywhere else (activeLane(), the Track
   Detail dock); the SET only matters to the arranger, which decides what a
   gesture acts on (app/timeline/CONTRACT.md inv. 12). Selection is NOT
   serialized and is not an action — it is view state.

Self-registration (Phase 5): sstdmixer.cpp and spluginchain.cpp register
"SStdMixer" / "SPluginChain" with SProjectLoader from static initializers.

How to test: render_sawtooth_with_effects.qxa; asset actions have roundtrip
coverage via action_roundtrip_test.

Known debt: mixer→timeline/pluginui edges (getDetailEditWidget creates
views) — the renderer/editor factory extraction is the Phase 6 fix.
