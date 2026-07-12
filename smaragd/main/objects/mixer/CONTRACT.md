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

How to test: render_sawtooth_with_effects.qxa; asset actions have roundtrip
coverage via action_roundtrip_test.

Known debt: mixer→timeline/pluginui edges (getDetailEditWidget creates
views) — the renderer/editor factory extraction is the Phase 6 fix.
