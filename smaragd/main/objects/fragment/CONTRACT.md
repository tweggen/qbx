# app/objects/fragment — CONTRACT

Purpose: `SLaneFragment` — a path container with NO TRACK IDENTITY (proposal
41 D1). An ordered set of clip links and a summing `twTrackMix` at unity as
its root component. No fader, no inserts, no instrument, no solo, no arm, no
meters, no automation of its own. It is what `SCut` windows, exactly as an
`SCut` today windows an `STrack` or the root `SStdMixer` — this module
EXTENDS what an asset may window; it does not add a parallel concept. The
verbs that create/place/remove one (`pack-clips`, `unpack-clips`,
`duplicate-asset-here`, `set-clip-event-channel`) live in `objects/mixer` and
`objects/cut`, not here — see the dependency note below for why.

Public headers: `app/objects/fragment/slanefragment.h`

Depends on (engine): `tw/core`, `tw/graph`, `tw/mix`, `tw/events` (the
`twEventClipSet` that backs the residual feed, M3). App edges: per
`tools/check_layering.py` — `objects/fragment` sits at the RANK of
`objects/track` (not inside it: a fragment must not inherit the track's
plugin/instrument dependencies) and NOT inside `objects/cut` (the dependency
runs cut → fragment — an `SCut` may window a fragment — never the reverse).
`objects/mixer` holds the `objects/fragment` edge instead (it already
depended on `objects/cut` for `create-asset`/`place-asset`) so that `pack-
clips`/`unpack-clips`/`duplicate-asset-here` can mint the registered `SCut` a
pack or a duplicate needs — minting a cut is exactly the thing THIS module
may not do (see inv. 2).

Design: `plan/proposed/41_LANE_FRAGMENTS.md` (D1–D15, the milestone list, and
the M2b correction to D3a). `main/model/CONTRACT.md`'s `isPathContainer()` vs
`isLane()` section (D3) is the load-bearing prerequisite reading.
`main/objects/track/CONTRACT.md` inv. 29 documents the residual-event-feed
DUAL-INSERT on the `STrack` side of D4. `main/objects/cut/CONTRACT.md`
documents `set-clip-event-channel` (D6) and the rate-refusal (D5) on the
`SCut` side.

## Invariants

1. **NO TRACK IDENTITY IS STRUCTURAL, NOT A CONVENTION** (D1). `SLaneFragment`
   has no fader, no plugin chain, no instrument slot, no solo/arm/edit-group
   state and no automation lane vector of its own — asserted in `fragment_test`
   by absence, not by comment. Its only state is the ordered child link list,
   the summing `twTrackMix` (`cpTrackMix_`), and (M3) its own `eventClips_`.

2. **OWNERSHIP IS THROUGH THE REGISTERED CUT, NEVER A SEPARATE REGISTRY**
   (D2). A fragment is constructed with no tree parent
   (`new SLaneFragment(project)`, the same shape `new SPlainWave(project)`
   uses) and its only reference into the live object graph is the unparented
   content `SLink` an `SCut`'s constructor builds over it
   (`SCut(project, *fragment)`). `SProject::registerAsset(name, cut)` — the
   SAME mechanism a container asset already uses — pins that cut with
   `addRef()`. Mixer CONTRACT inv. 2 applies unchanged: removing the last
   PLACEMENT does not delete the asset; `remove-asset` does, and the
   fragment goes with it because nothing else references it. There is no
   fragment-specific registry, and none should be built.

3. **SHARING IS THE INVARIANT AND IS NEVER BROKEN** (D2). One asset, N
   placements, every placement an `SLink` to the SAME registered `SCut` —
   the Unix hard-link model. An edit made through one placement's window is
   visible through every other placement, because there is only one window
   object to edit. **A variation is a NEW ASSET, never an un-shared
   placement**: `duplicate-asset-here` (`objects/mixer`) mints a second
   `SLaneFragment` by DEEP-COPYING the first (each child re-duplicated via
   `makeDuplicateClip`, content shared, window cloned), registers a second
   cut over the copy, and repoints exactly the ONE addressed placement to it.
   The original asset and every other placement of it are untouched, because
   they were never the thing being edited.

4. **`isPathContainer()` TRUE, `isLane()` FALSE, AND THE TWO MUST NEVER BE
   CONFLATED** (D3, M0's split — see `main/model/CONTRACT.md`). The index-
   path search and the placement service may descend into a fragment and it
   may be windowed as an asset (`isPathContainer()`), but it carries no
   solo/mute/edit-group/arm state and must never be consulted for any of
   them (`isLane()`). `STrack` and `SStdMixer` are still the only two types
   that answer both true; `SLaneFragment` is the first to answer them
   DIFFERENTLY, which is exactly the case the M0 split exists for — folding
   the two back together would let a fragment-internal flag darken lanes
   across the whole project it happens to be placed into.

5. **A FRAGMENT'S CHILDREN ARE ADDRESSABLE FOR PROPERTY EDITS, BUT `laneAt()`
   STAYS STRICT** (D3a, closed by M2b). `splacements::placementAt()` resolves
   a clip's PARENT through `containerAt()` (uses `isPathContainer()`, the
   strictly wider predicate inv. 4 names) rather than `laneAt()`, so
   `resize-clip`, `set-clip-volume`, `set-pitch`, `slip-clip` and every other
   clip-property verb can reach a clip already packed into a fragment, once
   addressed with the asset-name-qualified spelling `splacements::rootNamed()`
   gained in M2b: `<AssetName>:<idx>` resolves the qualifier to the asset's
   windowed fragment content (filtered to a non-lane path container, so an
   asset over a plain track gains no second name), tried only after the
   arrangement dict misses so an arrangement always wins a name collision.
   **`laneAt()` — the placement DESTINATION resolver `place-clip`/`move-clip`/
   `pack-clips`'s own lane check all use — is UNCHANGED and stays strict**: a
   clip can be deleted from a fragment (inv. 6) but can never be MOVED into or
   out of one by any verb but `pack-clips`/`unpack-clips`. This was the exact
   trade D3 makes: the predicate that stops a fragment's internal flags
   darkening lanes across the project (inv. 4) is the same predicate every
   clip verb used to decide what it may address, so widening it needed its
   own audit (M2b) rather than falling out of M1 for free.

6. **DELETION CASCADES, AND THAT IS INTENDED** (decided 2026-08-21, D3a).
   Because `placementAt()` is fragment-aware (inv. 5), any code path that
   resolves a clip through it and then deletes the resolved `SLink` — the
   production `unplace-clip`/`remove-midi-clip` (both live-only inverses of
   `place-clip`/`insert-midi-clip`, neither reachable from a fragment through
   any verb that can place INTO one today) and the M8 testkit gate
   `delete-fragment-clip` alike — removes the child from the ONE shared
   fragment (inv. 3), and the removal is therefore visible through EVERY
   placement of the asset. This is D2's invariant working, not an exception
   to it: "edit any placement and all change" applies to a deletion exactly
   as it applies to a resize or a volume change. Gate:
   `fragment_delete_cascades.qxa` (M8) — places the same asset twice, deletes
   one child through one address, asserts the RENDERED audio lost that
   material through both placements, and that one undo restores it through
   both.

7. **INVALIDATION REACHES A NESTED CLIP FOR FREE; MAKING THE EDIT AUDIBLE
   DOES NOT** (M2b's actual finding, correcting D3a's original guess). An
   `SCut`'s content link is an ordinary parented `SLink`
   (`scut.cpp:1346-1347`), so `SObject::invalidateRenderPathRange()`'s
   generic recursive walk descends into a fragment exactly as into any other
   child — this already worked with no fix. What did NOT work is
   `STrack::refreshClipGainCurves()`'s fragment equivalent: a track
   re-pulls each DIRECT child's `getVolume()`/`cut:Gain` curve into its OWN
   `twTrackMix`'s clip entries whenever its render-chain epoch bumps, and
   `SLaneFragment` had no such method — so `set-clip-volume` on a
   fragment-nested clip invalidated correctly (pages went stale, a MODEL-
   STATE assertion would pass) and then rendered the EXACT PRE-EDIT AUDIO,
   because nothing had ever told the fragment's `twTrackMix` the new gain
   existed. Fixed by giving `SLaneFragment` its own `refreshClipGainCurves()`
   (same shape, same call sites in `bumpRenderChainEpoch[Range]()`) — the
   missing HALF of the `STrack` precedent, not a reachability fix. This is
   why `fragment_nested_edit_reaches_all_placements.qxa` and
   `fragment_delete_cascades.qxa` both gate on RENDERED RMS through every
   placement, never on clip-window model state alone.

8. **EVENT CONTENT BUBBLES AS A RESIDUAL FEED, AND RESIDUAL-ONLY** (D4, M3).
   `SLaneFragment` owns its own `twEventClipSet eventClips_` — one entry per
   Event-kind child link, same key-by-`SLink*`, same window/note-off/loop
   machinery `STrack`'s own set already runs — and overrides
   `SObject::resolveEventFeed()` to flatten it into ONE immutable
   `twEventSeq` snapshot on EVERY call: no dirty-flag cache, because a
   fragment's children are write-once past `pack-clips` (inv. 5's addressing
   gap means nothing edits a fragment's CHILD LIST after packing except
   `unpack-clips`, which removes the whole fragment's worth at once) and
   this mirrors `STrack::eventFeed()`'s own "rebuilt on every read"
   discipline. The windowing `SCut` wraps that flattened, content-relative
   sequence with ITS OWN slip/loop map — naming only the base-class virtual,
   never `SLaneFragment` directly, because `objects/cut` may not depend on
   this module. `STrack::trackChildWasAdded` DUAL-INSERTS a
   residual-exporting placement into both `cpTrackMix_` (audio sum) and its
   own `eventClips_` (residual feed), keyed by the SAME `SLink*` (track/
   CONTRACT.md inv. 29). **A container asset (an `STrack`/`SStdMixer` placed
   elsewhere) exports NOTHING through this path**, because neither overrides
   `resolveEventFeed()` — the base default is "Event content only" and a
   container's own `contentKind()` stays Audio — so D4's "no double-trigger"
   rule costs zero special-case code, not a predicate that has to be called
   correctly at every site.

9. **THE CHANNEL REMAP LIVES ON THE PLACEMENT, NEVER THE SHARED CONTENT**
   (D6). `set-clip-event-channel` (`objects/cut`) writes
   `SLink::getEventChannelOverride()` — on the `SLink`, not the fragment or
   its cut — because D2 shares ONE `SCut` across every placement of an
   asset (inv. 3), and a remap stored on the content would move ALL of them
   at once. `-1` = as-authored, matching `midiOutChannel`'s convention
   (proposal 37 P7) so the scripting API speaks one dialect. `objects/cut`'s
   `resolveEventFeed` closure applies this override — it is the one place
   that already holds both the `SLink` and the resolved feed.

10. **A RATE ≠ 1 OVER EVENT-EXPORTING CONTENT IS REFUSED, NEVER
    DOUBLE-CONVERTED** (D5). POSITION_DOMAINS rule 7: the tick→frame
    conversion happens exactly once, inside `SMidiCut`. A stretched `SCut`
    windowing a fragment that exports events would convert twice, the
    second time in the FRAME domain — silently breaking tempo-follow, the
    Ardour ≤ 6 defect the tick-native model exists to avoid. Refused loudly:
    a log line naming the rate (`objects/cut`), never a silently-degraded
    render. Gate: `fragment_rate_refused.qxa`.

11. **A PURE-EVENT FRAGMENT BUILDS NO AUDIO CAPTURE** (D7, M4).
    `isPureEventContent()` answers true while `audioChildCount_` is zero
    (empty, or event-only) and short-circuits all FOUR of `SCut`'s
    UI-thread capture paths (`buildCapture_`, `ensureReader`,
    `invalidateAspects`, `getPreview`) — the same four call sites the
    `isLiveRecording()` precedent needed, budgeted for up front (proposal
    41 trap 2) because missing even one costs tens of milliseconds on the
    UI thread per pure-event fragment placed. Without it,
    `SCut::buildCapture_` treats an `SLaneFragment` with no random source as
    container-backed and renders a snapshot GUARANTEED to be silence.

12. **SINGLE-LANE BY CONSTRUCTION** (D8). A fragment holds clips from exactly
    one source lane; `pack-clips` refuses a selection spanning two lanes,
    naming both. Multi-lane reuse is a folder track (already works), not a
    fragment feature — a fragment spanning multiple instruments would need
    to decide which instrument each stream reaches, which means routing,
    which means track identity, and inv. 1 is gone.

13. **WIDTH FOLLOWS THE PROJECT, LIKE EVERY OTHER SUMMING PARENT**
    (`onProjectChannelsChanged`, proposal 36 B4 discipline). `cpTrackMix_`'s
    channel count tracks `SProject`'s channel count exactly as `STrack`'s
    and `SStdMixer`'s do — width-following only, never plumbed further: a
    fragment has no plugin chain to re-derive a channel-mismatch mapping
    for.

## Threading

Follows THREADING.md rule 2, same as `objects/track`: state mutated only on
the main thread through the child-link signal handlers
(`fragmentChildWasAdded`/`Removed`/`Moved`/`DurationChanged`, mirroring
`STrack`'s own, minus what a fragment cannot have — no live-recording
short-circuit, no nested-track mute/solo forwarding, D8). `cpTrackMix_` is a
`twComponent` and follows the engine's own freeze-time locking; nothing here
introduces a fragment-specific mutex. `eventClips_.collect()` (M3) runs on
whatever thread calls `resolveEventFeed()`, exactly as `STrack::eventFeed()`
does — the `twEventClipSet` machinery is already safe for that (events/
CONTRACT.md).

## How to test

`ctest -R "fragment_test|action_roundtrip_test"` plus the qxa cases:
`fragment_pack_roundtrip`, `fragment_place_reuse`, `fragment_duplicate_asset`,
`fragment_pack_multilane_refused` (M2); `fragment_nested_edit_reaches_all_
placements` (M2b, addressing + the invalidation gap, inv. 5/7);
`fragment_delete_cascades` (M8, the cascade, inv. 6); `fragment_midi_feed`,
`fragment_midi_no_double_trigger`, `fragment_midi_channel_remap`,
`fragment_midi_loop`, `fragment_rate_refused` (M3/M4, inv. 8-11);
`fragment_paint_disjoint`, `fragment_tag_drag` (Part B — the visual model,
gated in `main/timeline/CONTRACT.md`, not here: painting and hit-testing are
`objects/track`/`timeline` territory even when the clip being painted windows
a fragment).

## Known debt

- **A packed fragment is write-once for STRUCTURE.** Property edits on a
  nested clip work (inv. 5/7); moving material into or out of one, or
  reordering its children, does not — `unpack-clips` is the only way out.
  The proposal's own note: the real fix is an "open the asset" affordance
  that makes the fragment the addressing ROOT (as an arrangement tab already
  does for a nested root), which is out of scope for the milestones executed
  so far.
- **No per-placement window.** D2 is deliberate — a variation is a new asset
  (inv. 3) — but "make this one placement independent without minting a
  second asset name" (`make-unique` in the M8 task framing) has no verb;
  `duplicate-asset-here` is the only route, and it always names and
  registers a new asset.
- **Nested fragments beyond the existing cycle guard** are unexplored beyond
  what `sarrangements::reaches` already allows (no new depth rule was
  written).
- **Automation lanes on a fragment** do not exist (non-goal — a fragment has
  no `self:` targets, D1). A `cut:Gain` envelope on a child clip's own
  window still travels with that window via `refreshClipGainCurves()`
  (inv. 7).
