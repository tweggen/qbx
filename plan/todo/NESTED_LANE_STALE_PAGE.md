# Nested-lane stale page — ROOT CAUSE FIXED 2026-08-09

**Status:** the headline bug is FIXED and gated. Several smaller defects found
during the investigation are still OPEN — see "Still open" at the bottom.

## The bug

Under a folder track, an edit on a nested lane was not heard: the deleted clip
kept playing, the per-track VU meter kept moving, and the first frozen page that
only PARTIALLY covered the edit was never re-rendered — it served its pre-edit
content forever, byte-identically on every subsequent render. A clip added at
frame 96000 stayed silent until exactly 131072 = 2 × 65536, a page boundary.
The identical script with the nesting removed was always correct.

## Root cause

`twStreamingLatch::copyData` judged staleness on the page's OWN `contentEpoch`
stamp:

```cpp
const uint64_t epochNow = getComponent()->contentEpochNow();
...
if (!page || page->startPosition != pageStart || page->validAspects == 0 ||
    page->contentEpoch.load() < epochNow) {
```

But the stamp is written by whichever component actually RENDERED the page
(`twtrackmix.cc:350`, `page->contentEpoch.store(contentEpochNow())`), and that is
not `getComponent()`. With no inserts, `twPluginChain` forwards its upstream
`twTrackMix` page through verbatim (`twpluginchain.cc:242-252`), so the page
arrives carrying the TRACKMIX's counter while the gate compares it against the
CHAIN's. The two are independent per-component atomics — both start at 1, and
`twTrackMix` self-bumps on `insertClip` / `removeClip` / `updateClip` /
`setClipMuted` / `setTrackGain`, none of which touch the chain. So the trackmix
counter runs permanently ahead and `page->contentEpoch < epochNow` could never
fire. **The staleness test was dead code.**

Measured directly, tracking one component across the edit:

```
render 1:  comp=…170eb0 pageStart=65536 epochNow=6  heldStart=0      heldEpoch=9
render 2:  comp=…170eb0 pageStart=65536 epochNow=7  heldStart=65536  heldEpoch=9   <-- reused
```

`heldEpoch (9) >= epochNow (7)` → gate passes → stale page re-served, then
re-stamped current. Note the second fact that made the fix possible: the
component's OWN epoch does move across the edit (6 → 7), so a like-for-like
comparison on a single counter is both available and sufficient.

Why only nested? The reuse branch requires `held->startPosition == pageStart` —
the reader being parked on exactly the page being re-frozen. Instrumented counts:
**top-level 0 hits, nested 1 hit, at exactly the bad page.**

## The fix

`twLatchStreamingOutput` now remembers the epoch it OBSERVED on the producer when
it accepted its held page (`previousPageEpoch_`, threaded through `copyData` as
`readerPrevEpoch`), and re-validates on `observed != contentEpochNow()`. One
counter compared against itself cannot drift. The same rule replaced the
predecessor/`chainFrom` test.

A page served from a scheduler binding (`twFrozenInputScope`) is deliberately
recorded as epoch 0, not as current: it is trusted only for the render that bound
it, so blessing it would let a later unbound call reuse it with nothing left to
validate. `mix_test`'s "empty set falls back to the legacy pull" is the gate for
that, and it caught the first version of this fix.

Recorded as invariants 5 and 6 in `smaragd/tw303a/graph/CONTRACT.md`.

## Proof: controlled A/B

The bug is **deterministic**, not intermittent. Reverting ONLY the gate condition
(all other plumbing identical), rebuilding, and running the repro five times each
way, back to back, measuring RMS over `[96000,131072)` of `a_post.wav`:

| gate | run 1 | 2 | 3 | 4 | 5 |
|---|---|---|---|---|---|
| `heldEpoch != epochNow` (fixed) | 0.0487 | 0.0487 | 0.0487 | 0.0487 | 0.0487 |
| `page->contentEpoch < epochNow` (pre-fix) | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |

An earlier investigation concluded the failure was intermittent — it reproduced
for ~15 minutes and then stopped across ~45 further runs. That was an artifact of
this same binary being rebuilt underneath that investigation while it ran
(probes in, probes out, then the fix). **Lesson for anyone bisecting engine
behaviour here: only one agent/session may drive `build/` at a time, or the
"experiment" is measuring whatever binary happened to be on disk.**

## Gates

- `qxa.delete_clip_in_group` — its `[96000,144000)` band was commented out while
  this bug was open; it is now restored and passing.
- `mix_test` (invariant 6).
- The minimal repro below now matches the top-level control exactly.

```xml
<add-track index="-1"/>                       <!-- 0: becomes the folder -->
<add-track index="-1"/>                       <!-- 1: gets nested -->
<add-sample trackPath="1" filePath="../test_sawtooth.wav" timePos="0"/>
<split-clip clip="1,0" splitTime="96000"/>
<remove-sample trackPath="1" clipIndex="1" timePos="96000"/>
<reparent-track source="1" destParent="0" destIndex="-1"/>
<render filename="a_pre.wav" format="wav" quality="10"/>   <!-- freezes the pages -->
<add-sample trackPath="0,0" filePath="../test_sawtooth.wav" timePos="96000"/>
<render filename="a_post.wav" format="wav" quality="10"/>
```

Per-12000-frame RMS of `a_post.wav`:

| | 96000 | 108000 | 120000 | 132000 |
|---|---|---|---|---|
| before the fix | 0.0 | 0.0 | 0.0237 | 0.1014 |
| after the fix | 0.0167 | 0.0441 | 0.0726 | 0.1014 |
| top-level control | 0.0167 | 0.0441 | 0.0726 | 0.1014 |

## Still open

Found while investigating, none of them the headline cause, all confirmed by
reading the code:

1. ~~**Readahead demands are epoch-blind.**~~ **FIXED 2026-08-09.**
   `audio_engine.cc` gated re-demand purely positionally, so an edit made while
   a demand was in flight issued no new demand and up to a whole readahead
   window (~4 s) of pre-edit audio kept playing — which is why edits and
   solo/mute clicks read as ignored rather than late. `pendingDemand_` now
   carries `pendingDemandEpoch_`, the content epoch it was issued against, and
   coverage requires `pendingDemandEpoch_ == epochNow`; a superseded demand is
   dropped and its pages are rejected by the per-page validity check. The
   header comment had promised this ("or the wanted window/epoch moved on")
   since the demand consumer was written — only the window half was implemented.
   *No bespoke automated gate:* this is a LATENCY property of the live-playback
   path, and the qxa suite drives offline renders. A timing assertion tight
   enough to separate the two behaviours would be flaky, which is worse than
   none here. The change can only cause MORE re-demands, never fewer, so its
   failure mode is extra scheduling work rather than wrong audio. Gating it
   properly needs a scheduler-driven playback test asserting that a demand
   issued pre-edit is superseded — `playback_test` can link `tw_schedule`
   already, so the vehicle exists.
2. **`twTrackMix::updateClip` stops at the first matching key** (`break;` at
   twtrackmix.cc:204) while `removeClip` loops over all of them. Any path that
   inserts a duplicate entry for one `SLink*` leaves a second entry frozen at its
   old extent.
3. **`STrack::setNBusses`'s initial-sync loop** (strack.cpp:399-414) re-inserts
   every existing child but wires none of the `durationChanged` /
   `startTimeChanged` connections that `trackChildWasAdded` makes.
4. **`twTrackMix` has no `invalidatePagesInRange` override** (unlike
   `twPluginChain`, twpluginchain.cc:296-306), so an external invalidation never
   clears a clip entry's `previousPage`. Only `updateClip` does.
5. **The scheduler's verify-at-publish retry is a no-op**:
   `capture_revalidator.cc:404-414` re-renders through `twComponent::freezePage`,
   which cache-hits the page attempt 1 just wrote and stamped current
   (twcomponent.cc:556-561). A mid-render edit cannot be repaired by the retry.
6. **Readahead poisons the root cache**: `audio_engine.cc:674` calls
   `getOrAllocatePage(pos)`, inserting an empty placeholder that `freezePage`
   later reuses, so `stalePredecessor` is never set for that position and the
   proposal-16 fallback degrades to a dropout there. *Fix:* use the
   non-allocating `getPageIfExists` in the readahead probe.
