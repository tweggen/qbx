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

**M2 (pack-clips / unpack-clips / duplicate-asset-here) — executed.** The
three verbs live in `objects/mixer` (`spackclipsaction.{h,cpp}`,
`sunpackclipsaction.{h,cpp}`, `sduplicateassethereaction.{h,cpp}`), NOT here:
`check_layering.py`'s `objects/fragment` entry still names no edge to
`objects/cut`, and minting the registered `SCut` a pack or a duplicate needs
is exactly the thing this module may not do (see the dependency note above).
`objects/mixer` gained the `objects/fragment` edge instead — it already
depended on `objects/cut` for `create-asset`/`place-asset`, so it is the
natural home `slanefragment.h`'s class comment already named. `SObject`
gained a public `refCount()` (model/CONTRACT.md territory, not this module's)
so `unpack-clips` can refuse when a fragment's asset has more than one
placement rather than emptying it out from under a sibling.

Gate: `ctest -R "fragment_test|action_roundtrip_test"` plus the qxa cases
`fragment_pack_roundtrip`, `fragment_place_reuse`, `fragment_duplicate_asset`,
`fragment_pack_multilane_refused`.

**M2b (addressing inside a fragment) — executed.** D3a's gap: `resize-clip`,
`set-clip-volume` and every other clip verb could not reach a clip already
packed into a fragment, because they all resolve through
`splacements::placementAt()`, which required the clip's immediate parent to
answer `isLane()` — a fragment answers that false by design. This module gets
NO new code from M2b (the fix is entirely in `model` and one caller in
`objects/cut`): `splacements.h` gains `containerAt()` (uses
`isPathContainer()`, the strictly wider predicate) and `placementAt()` now
resolves the clip's PARENT through it, while `laneAt()` — the placement
DESTINATION resolver `place-clip`/`pack-clips`'s own lane check use — stays
untouched, so a clip still cannot be moved into or out of a fragment
(`unpack-clips` remains the only way out). `splacements::rootNamed()` also
gains an asset-name fallback: `<AssetName>:<idx>` addresses the fragment's own
children (`SClipWindow::of(asset)->windowContent()`, filtered to a non-lane
path container so a container asset over a plain track gains no second name),
tried only after the arrangement dict misses so an arrangement always wins a
name collision.

**The invalidation trap this module actually had**, found empirically (a
model-only assertion would have missed it): `SObject::invalidateRenderPathRange`
*does* reach a clip nested in a fragment — the fragment's registered `SCut`'s
content link is an ordinary parented `SLink` and the walk descends into it
like any other child, so bumping the epoch on every ancestor, through EVERY
placement, already worked. What did not exist was the fragment's half of
`STrack::refreshClipGainCurves()`: a track re-pulls each DIRECT child's
`getVolume()`/`cut:Gain` curve into its OWN `twTrackMix`'s clip entries every
time its render-chain epoch bumps, and `SLaneFragment` had no such method at
all — its `bumpRenderChainEpoch[Range]()` overrides only bumped
`cpTrackMix_`'s page cache. A resize (which restructures the mix's clip entry
via `insertClip`/`updateClip` directly) therefore already worked through every
placement; `set-clip-volume` on a fragment-nested clip changed the model and
invalidated every placement's pages, and then rendered **exactly the
pre-edit audio anyway**, because nothing had ever told the fragment's
`twTrackMix` the new gain existed. Fixed by giving `SLaneFragment` its own
`refreshClipGainCurves()` (same shape, same call sites in the epoch-bump
overrides) — the missing half of the STrack precedent, not a reachability
fix.

Gate (added): the qxa case `fragment_nested_edit_reaches_all_placements`
(`resize-clip` and `set-clip-volume` on `Riff:0`/`Riff:1`, both addressed by
asset-name qualifier, both audible through TWO independent placements —
gated on rendered RMS, not model state) and `fragment_test`'s
`testAssetNameAddressing` (the resolver contract: `laneAt()` unchanged,
`containerAt()`/`rootNamed()`/`placementAt()` reach a fragment's children,
arrangement wins a name collision, an asset over a plain lane gains no second
name).

**M3 (event bubbling) / M4 (no silent capture) — executed.** `SLaneFragment`
gained its OWN `twEventClipSet eventClips_`, one entry per Event-kind child
(same key-by-`SLink*`, same window/note-off/loop machinery STrack's own set
already runs), and overrides `SObject::resolveEventFeed()` to flatten it into
ONE immutable `twEventSeq` snapshot on every call — no dirty-flag cache, since
a fragment's children are write-once past M2's pack-clips (D3a) and this
mirrors `STrack::eventFeed()`'s own "rebuilt on every read" discipline. The
windowing `SCut` (`objects/cut`) wraps that flattened, content-relative
sequence with ITS OWN slip/loop map — never naming `SLaneFragment`, only the
base-class virtual, because `objects/cut` may not depend on this module (see
the dependency note above: cut -> fragment is the CONCEPTUAL direction, not a
declared `check_layering.py` edge) — and refuses (D5) rather than
double-converts a non-unity rate. `STrack::trackChildWasAdded` DUAL-inserts a
residual-exporting placement into both `cpTrackMix_` (its audio sum) and its
OWN `eventClips_` (the residual feed), keyed by the same `SLink*`; a container
asset (an `STrack`/`SStdMixer` placed elsewhere) answers `resolveEventFeed()`
with nothing because neither overrides it (D4, "exports nothing" is free, not
a special case). `isPureEventContent()` (true iff no audio-content child was
ever inserted into `cpTrackMix_`) short-circuits all four of `SCut`'s
UI-thread capture paths (`buildCapture_`, `ensureReader`, `invalidateAspects`,
`getPreview`) over a pure-event fragment, mirroring the `isLiveRecording()`
precedent one for one.

Gate: `ctest -R "fragment_test|action_roundtrip_test"` plus the qxa cases
`fragment_midi_feed`, `fragment_midi_no_double_trigger`,
`fragment_midi_channel_remap`, `fragment_midi_loop`, `fragment_rate_refused`.
