# app/objects/track — CONTRACT

Purpose: the track object. STrack (bus mixers, clip synchronization to
twTrackMix, MapPosFn wiring), its renderers, strackpath (path resolution
used by ALL placement actions), and track/placement actions: add/remove/
move/reparent/restore-track, set-track-volume, move-clip, remove-clip.

Public headers: app/objects/track/*.h

Depends on (engine): tw/core, tw/graph, tw/mix, tw/plugins. App edges: per
tools/check_layering.py.

Invariants (normative detail: CLIP_MODEL.md):
1. Clips are keyed by SLink* in every insertClip/updateClip/removeClip —
   never by component.
2. Every insertClip passes the mapPosFn
   (child.getSObject().mapTimelineToComponentPos) — a clip inserted without
   it plays from source position 0.
3. trackChildDurationChanged: sender() is the SObject — resolve links by
   scanning childLinks(); update EVERY link referencing the sender.
4. trackChildWasMoved: sender() IS the SLink (startTimeChanged is a link
   signal).
5. strackpath resolution: comma-separated child indices from the root
   mixer; reparent guards against self/descendant cycles.

How to test: render_split_slip_offset.qxa (move-clip across tracks +
removeClip), render_sawtooth_with_effects.qxa (reparent),
test_track_*.qxa (UI).

Known debt: strackpath being here forces objects/track edges from every
action slice — a path-resolution service extraction is a Phase 6 candidate.
