# Concept: Grain Playback (time-stretch / pitch-shift / grain clouds)

Design only — structure and placement first, advanced DSP deferred. Answers the
two opening questions: **where do instances live**, and **what is the coarse
structure**. The source-vs-instance question is decided here (with rationale),
because *placement depends on it*.

## TL;DR

- Build one engine node, `twGrainPlayer` (a rewrite of the half-finished
  `twGrainer`), that reads a *source* `twComponent` and re-emits it through a
  grain engine: a **time map** (output time → source read position), a
  pluggable **slicer** (time-slice / transient / hybrid — one selected at a
  time), and an **overlap-add renderer** fed from a **sliding source cache**.
- **It is an instance modifier, owned per `SCut`** — not a source modifier on
  `SPlainWave`. The deciding factor is mechanical, not stylistic: a grain node is
  *stateful* (one read cursor, one cache) so it can be pulled by exactly **one**
  stream. A source is shared by many cuts; a shared node cannot serve them at
  independent positions. Source-level settings, if wanted, become *defaults the
  per-cut node inherits*, never a shared node. (§2)
- Insertion point is tiny and mirrors an existing pattern: `SCut::getRootComponent()`
  today delegates straight to its content; instead it lazily creates and owns a
  `twGrainPlayer` whose source is `content_->getRootComponent()`. When grain
  playback is identity (stretch 1, pitch 0, no warp) it stays a passthrough —
  zero behaviour change, incremental adoption. (§3)
- **Per-cut owns the cursor, not the computation.** The expensive work (analysis,
  rendered grains) is a *pure function of deterministic parameters*, so it is
  hoisted into shared, parameter-keyed caches; the per-cut node becomes a thin
  scheduler + cache reader. Identical cuts (a loop copied 200×) hit the cache and
  are computed once. (§7)

---

## 0. What the engine already gives us

The pull model (`twComponent::calcOutputTo(dest, length, idx)` + `seekTo` +
`isSeekable`) is exactly the substrate this needs:

- `twWavInput` **is seekable** (`isSeekable()==true`, `seekTo`) and already keeps
  a sample cache — so non-monotonic source access (grain overlap, reverse,
  jumps) is *possible*, just not free.
- `twTrackMix::calcOutputTo` already pulls clips by calling
  `lk->getRootComponent().calcOutputTo(...)` **directly** (a raw component
  pointer), not through formal input plugs. The existing `twGrainer` follows the
  same idiom (`sourceComponent_` pointer, set via `setSourceComponent`). So the
  grain node reading its source by a direct `twComponent*` is consistent with the
  codebase — no plug wiring required for the first cut. (Plug/latch wiring stays
  available as the "proper" tw303a path if we later want it negotiable.)
- Format/rate negotiation (proposal 04) already exists; the grain node slots in
  with the **default** `narrowCaps` (couples in/out to one common rate). Per-grain
  pitch is done by *reading the cache faster*, not by changing stream rate, so
  the node neither resamples nor rate-decouples at the stream boundary. (§5)

The existing `twGrainer`/`twGrainSpec` are the right *idea* but the body is a
stub (`findValidGrain` returns −1; the fade-in branch never writes output;
pitch ignored). Treat them as reference, not foundation — rewrite.

---

## 1. Coarse structure of the node

`twGrainPlayer` decomposes into four collaborators it owns, so the giant
`calcOutputTo` becomes a thin loop over clean pieces:

```
twGrainPlayer (twComponent)
 ├─ source        twComponent*          the material being grained (pulled directly)
 ├─ twSourceCache  sliding window over source: linear pull + seekTo on miss
 ├─ twTimeMap      output time t → source read position; holds rate/stretch/pitch curve
 ├─ twGrainSlicer* STRATEGY (one selected): timeslice | transient | hybrid
 │                  → produces grain boundaries (a live/precomputed twGrainSpec)
 └─ overlap-add renderer: window + crossfade grains from cache into output
```

Responsibilities, separated so each is testable and later automatable:

1. **`twTimeMap`** — the "playback rate at output time *t*" function the user
   described, *integrated* to a source cursor. This is what decouples output
   length from source length (= time-stretch) and what a pitch curve drives.
   Identity map ⇒ passthrough.
2. **`twGrainSlicer`** (abstract; `twTimeSliceSlicer`, `twTransientSlicer`, …) —
   "one or more algorithms predefined, one selected at a time." Given a source
   region it yields grain boundaries. Time-slicing is computable on the fly;
   transient detection is an offline/precompute step over the source. Output is
   the existing `twGrainSpec`/`twSingleGrainSpec` representation (keep it — it is
   a fine schedule data structure).
3. **`twSourceCache`** — the mandated cache. Grains overlap and may reach
   backward or be reordered, while the source prefers forward reads. The cache
   holds a sliding window around the current read region, refilled by linear
   pulls; a grain needing data outside the window triggers `source->seekTo()` +
   refill (the escape hatch the seekable source affords). Sizing: a few × max
   grain length × playback-rate headroom.
4. **Overlap-add renderer** — windowing + crossfade (the salvageable core of the
   current `calcOutputTo`), reading grains from the cache, honoring per-grain
   pitch via simple interpolation / `twResampler`.

### 1.1 Per-channel subtlety (must design in from the start)

`calcOutputTo(dest, len, idx)` is called **once per channel per block**. The grain
schedule and time map **must be identical across channels** or the stereo image
smears. So: compute the time-map advance + grain schedule **once per block**
(on the first channel touched / a block-id guard) and **reuse** it for every
`idx`; only the cache and overlap-add buffers are per channel. This is a real
constraint the old single-channel stub never faced.

---

## 2. The placement decision: instance modifier, per `SCut`

The user framed it as instance (`SCut`) vs source (`SPlainWave`). Decided:
**instance**, for a mechanical reason plus a musical one.

**Mechanical (the real driver).** A grain node carries a read cursor (`pos_`) and
a cache — it is single-stream state. A `twComponent` that advances internal state
on `calcOutputTo` can be pulled by **one** consumer per linear pass. A source is
referenced by many `SCut`s across many tracks (that is the whole point of
`SLink` sharing). One shared grain node therefore cannot serve them at
independent positions/stretches — they would fight over `pos_` and the cache,
exactly the latent race already noted on the shared `twWavInput`. To make grain
playback a *source* feature you would have to turn the source into a **factory**
that mints a fresh reader node per consumer — a much larger refactor that also
fights the current "one shared `twWavInput` per `SPlainWave`" model. Not now.

**Musical.** Stretch / pitch / grain-cloud density are per-clip decisions (the
Ableton "warp per clip" model). `SCut` is *already* the per-instance object with
its own timing (`startOffset_`, `cutDuration_`, `loopStart_`) and is exactly the
1:1 boundary with a playback stream in the current graph. The grain node belongs
where the per-instance state already lives.

**Source-level settings, if ever wanted**, are expressed as **defaults** stored
on the `SPlainWave` that each `SCut`'s grain node copies on creation — never a
shared node. So "applies to all" is achievable without shared mutable state.

**Caveat (now addressed by proposal 07):** the per-cut grain node pulls the
*shared* `twWavInput`, so two grain nodes over the same wave race on the
underlying file/cache position. The node's own cache + `seekTo`-before-refill only
*narrows* the window. The real fix — splitting immutable sample data from a
per-consumer cursor (source-as-reader-factory) — is **proposal 07**, which also
makes time-stretch apply to *any* component placed before an `SCut`, not just
files. 07 steps 1–4 are prerequisites for the grain work landing cleanly.

---

## 3. Where instances live & the insertion point

Ownership mirrors `SPlainWave`-owns-`twWavInput`:

| Object | owns | lifetime |
|--------|------|----------|
| `SCut` | `twGrainPlayer *grain_` (lazy) | created on first enable / `getRootComponent`; deleted in `~SCut` |
| `twGrainPlayer` | its `twSourceCache`, `twTimeMap`, current `twGrainSlicer`, `twGrainSpec` | with the node |
| `SCut` | grain **parameters** (stretch, pitch, grain size, density, window, slicer id) | serialized in `serializeSelfAttributes`, read live by the node |

The one code change at the seam — today:

```cpp
twComponent &SCut::getRootComponent() { return content_->getRootComponent(); }
```

becomes: if grain playback is enabled, lazily build/return an owned
`twGrainPlayer` whose source is `content_->getRootComponent()`; otherwise keep
delegating directly (passthrough, no node, no cost). Parameters live on `SCut`
and are read live by the node every block — same pattern as `twTrackMix` reading
`track_.getVolume()` live, which is what makes them automatable later for free.

Register the node with the environment following whatever pattern the other
components use (`tw303aEnvironment::addModule`) so lifetime/teardown is uniform.

---

## 4. Seek / time model (the part that actually changes semantics)

Today `SCut::seekTo(off)` forwards `off + startOffset_` straight to the source —
output time *is* source time. With a grain node that identity breaks, and that is
the whole feature:

- The track gives the cut an **output** (clip-relative) position. The grain node's
  `twTimeMap` integrates the rate curve to find the **source** position;
  `startOffset_` becomes the time map's input anchor.
- Stretch decouples lengths: `SCut::getDuration()` (currently the standalone
  `cutDuration_`) must eventually be derived as `sourceSpan × stretch` via the
  time map. For the first structural cut, keep `cutDuration_` authoritative and
  let the time map fill the window; wire the derivation in when stretch lands.

This is the only place existing behaviour changes; everything else is additive.

---

## 5. Format / rate / channels notes

- **Rate:** node uses the default `narrowCaps` (one common rate in/out). Pitch is
  cache-read-rate, not stream rate — the node is not a resampler at the boundary.
  If the source is a foreign-rate `twWavInput`, proposal-04 negotiation already
  reconciles it upstream; the grain node sees canonical mono Float32 like the
  rest of the graph.
- **Channels:** keep the engine's mono-per-`idx` call convention. `nChannels` on
  the node mirrors source channels; per-channel cache + shared per-block schedule
  (see §1.1).

---

## 6. Suggested phasing (structure-only milestones)

1. **Skeleton + passthrough.** `twGrainPlayer` with source pointer, identity
   `twTimeMap`, no slicing → bit-exact passthrough. `SCut` owns it; default
   disabled so nothing changes. Proves the seam and lifetime.
2. **Source cache + single time-slice slicer + overlap-add.** Fixed grain size,
   stretch = 1: validates cache/window, per-channel schedule reuse, crossfade.
3. **Time map with constant rate** → time-stretch and (cache-rate) pitch-shift.
4. **Slicer strategy interface** + transient slicer; selectable.
5. **Parameter serialization on `SCut`**, then (later track) automation — the
   live-read design means automation is mostly plumbing, not redesign.

Everything past phase 2 is the "advanced stuff" the user asked to defer; phases
0–2 are the structural commitment this document recommends.

---

## 7. Sharing & caching across cuts (the "Funky Drummer" problem)

Motivating case: import a 6-minute break, slice a handful of hits (hi-hats,
snares, kicks), build a one-bar loop, then copy that bar across 7 minutes. With
deterministic constant stretch, every copy of a given hit is **byte-identical**.
Recomputing each is pure waste. The fix is to make *identity of parameters* the
cache key — not identity of `SCut`.

This **refines, not reverses, §2.** The "node is stateful ⇒ per-cut" argument was
only ever about the **playback cursor**. It says nothing about the **computed
material**, which is a pure function of deterministic inputs. Split them:

- **playback intent / cursor** → genuinely per-cut, cheap, ephemeral.
- **computed material** (analysis + rendered grains) → hoisted out of the cut
  into shared, parameter-keyed caches.

### 7.1 Three cache tiers, by what each depends on

1. **Decoded source samples** — already per-source: every cut's node pulls the
   *same* `twWavInput` cache on `SPlainWave`. Do **not** duplicate per cut. (The
   shared-cursor race from §2 is the only blemish on this tier — removed by
   proposal 07, which turns this tier into a stateless `twRandomSource`.)
2. **Source analysis** — transient map, slice-point candidates, envelope/preview.
   Pure function of *(source bytes, slicer config)* — independent of stretch,
   pitch, position. Compute **once per source**, store at the
   `SExternFile`/`SPlainWave` level, share across all cuts. Cheap; safely
   **persistable** with the project.
3. **Rendered grains / warped chunks** — the expensive overlap-add output. Pure
   function of *(source region, pitch, internal rate, window)* — **not** of which
   cut or where on the timeline. A **project/app-level content-addressed LRU
   cache**, owned by no cut. The tier that kills the wasted cycles. **Not
   persisted** (cross-machine FP determinism is dicey, and it is large).

### 7.2 The unit that maximizes tier-3 reuse: a "warped source"

For the constant-stretch case there is a clean conceptual object:

> **WarpedSource = (source, slicer, constant stretch, pitch)** → a deterministic,
> re-timed *infinite* stream, lazily materialized in cached chunks.

Every `SCut` with matching params is then just a `[start, len)` **window** into
that stream, so all copies of a hit read the same chunks → computed once. (This
is the Ableton model: the warped sample is the cached artifact; clips are views.)
When stretch later becomes *automated* (variable rate) the affine sharing breaks,
but tier 3 still helps: the rendered *grain material at a given pitch* is shared;
only the *schedule* (selection/spacing) differs per cut.

### 7.3 Consequences for the design

- **`twGrainPlayer` becomes thin:** cursor + param reference + cache handle. It
  *schedules and reads*; it no longer *owns* heavy data. Many cuts → many cheap
  nodes + one shared cache.
- **Params must be canonical + hashable** so identical intent → identical key.
  Copy-clip already copies params verbatim via `serializeSelfAttributes`, so
  copies hit the cache for free — exactly the motivating case.
- **Cache is optimization, never correctness.** Every lookup is compute-on-miss;
  bounded by an LRU memory budget; degrades gracefully under pressure. Grain
  clouds can generate huge counts — the budget is mandatory.
- **Invalidation:** source reload / slicer-param change invalidates tier 2;
  tier-3 keys include the source content hash + slice params, so stale entries
  become unreachable and age out (no explicit cascade needed). Hook to the
  existing reload / `renegotiationRequired` signals.
- **Cache manager ownership:** lives at project (or app) scope, beside the
  environment — *not* on any `SCut` or `SPlainWave`, since its whole point is
  cross-cut sharing.
```
