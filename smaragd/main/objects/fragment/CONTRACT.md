# app/objects/fragment — CONTRACT (stub)

**This is a minimal stub.** The full contract (invariants, threading notes,
how-to-test) lands with proposal 41 M8. Recorded here now so the module is
not undocumented in the meantime.

Purpose: `SLaneFragment` — a path container with NO TRACK IDENTITY (proposal
41 D1). An ordered set of clip links and a summing `twTrackMix` at unity as
its root component. No fader, no inserts, no instrument, no solo, no arm, no
meters, no automation of its own. It is what `SCut` windows, exactly as an
`SCut` today windows an `STrack` or the root `SStdMixer`.

**Ownership (decided, proposal 41 D2):** a fragment is owned by the
project-wide ASSET LIST, through the SAME `SProject::registerAsset(name, cut)`
mechanism a container asset already uses — the thing registered by name is
always the windowing `SCut`, never the fragment. The fragment has no
independent tree position; its only reference into the live object graph is
the unparented content `SLink` the registered `SCut`'s constructor builds
over it. Mixer CONTRACT inv. 2 applies unchanged: removing the last placement
does not delete the asset; `remove-asset` does, and the fragment goes with it
because nothing else references it. Sharing is the invariant (D2): one asset,
N placements, edit any and all change. A variation is a NEW asset — M2 deep-
copies the fragment and registers a second cut over it, rather than un-sharing
a placement. See `slanefragment.h`'s class comment for the full reasoning,
including why nothing here blocks that deep copy (flagged for M2, not solved
by M1).

Public headers: `app/objects/fragment/slanefragment.h`

Depends on (engine): `tw/core`, `tw/graph`, `tw/mix`. App edges: per
`tools/check_layering.py` — `objects/fragment` sits at the RANK of
`objects/track` (not inside it: a fragment must not inherit the track's
plugin/instrument dependencies) and NOT inside `objects/cut` (the dependency
runs cut -> fragment — an `SCut` may window a fragment — never the reverse).

See `plan/proposed/41_LANE_FRAGMENTS.md` for the design (D1-D15) and the
milestone list. `main/model/CONTRACT.md`'s `isPathContainer()` vs `isLane()`
section (proposal 41 D3) is the load-bearing prerequisite reading.

Gate: `ctest -R fragment_test`.
