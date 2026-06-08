# 10 — Render Cache / Recursive Capture (the "virtual memory" model)

**Status:** Design. The immediate increment (recursive capture, Phase 1) is the
agreed next step; it fixes the nested-group asset-preview bug recorded in STATE.md
("Asset preview … PARTIAL"). The later phases are the north star the user named.

---

## 1. The recurring pattern (why this proposal exists)

Almost every audio fix in this codebase has been the **same move**: take a
*stateful, forward-only, cursor-bearing stream* and turn it into *stable,
random-access, shareable data*.

- `twSampleSource` — WAV decoded into resident planar Float32 (proposal 07).
- `twResampledSource` — a sample rate-converted once into a resident buffer.
- `twGrainSource` — grain/overlap-add render materialised once.
- `twCapturingSource` — a container sub-arrangement rendered once into a buffer
  (proposal 07 step 5).

They all implement the one interface — `twRandomSource`
(`read(offset,dest,len,ch)`, `length`, `channels`, `sampleRate`,
`isReproducible`). **`twRandomSource` is already the "memory-mapped view"
abstraction.** What's missing is a *generic page-cache* behind it, so we stop
hand-rolling a bespoke "materialise once" class per feature.

### The Unix-VM analogy (the user's framing — keep as the design compass)

| Unix VM | Smaragd render cache |
|---|---|
| page | a fixed-size block of rendered frames |
| page fault | a read of an uncached block renders it from the producer |
| page cache | shared store of blocks keyed by (producer identity, block #) |
| `mmap` | a consumer holds a `twRandomSource` view; reads fault pages in |
| copy-on-write / invalidation | pages shared until an edit dirties them |
| content-addressing | identical sub-arrangements share one page set (dedup) |

## 2. Why this dissolves the nested-capture bug

The nested-group bug (STATE.md) exists **only because rendering is
cursor-streamed**: a parent `twTrackMix` sums a child *track* by pulling the
child's **rewire**, whose `twStreamingLatch` buffers ~16384 frames ahead, while
the parent **re-seeks each child every buffer**. Read-ahead vs. per-buffer
re-seek are irreconcilable for a track-of-tracks → one sub-track wins, the other
goes silent.

Make rendering **block-addressed** instead — "give me block *k* of your output" —
and a group's block *k* is just **the sum of its children's block *k***, each
faulted (recursively) from the cache. No latch, no re-seek, nothing to fight.
**Random access composes; streams don't.** That is the entire disease and the
entire cure.

## 3. Phased plan

### Phase 1 — Recursive capture (DO NOW; fixes the bug)

A `twRandomSource` that materialises a container's render by **recursively**
summing child random-sources, never pulling a live streaming rewire:

- For a container `C` (track or mixer), produce frames `[0, dur)`:
  - For each child link `(obj, startTime, dur)`:
    - **sample-like** child (`obj.getRandomSource() != NULL`): read it directly.
    - **track/container** child: obtain *its* recursive capture (Phase-1 applies
      to it too) and read that.
  - Mix child frames into the output at `startTime` (`+=`).
  - Apply `C`'s own **gain & mute** (mirror `twTrackMix::calcOutputTo`'s
    `factor = isMuted ? 0 : pow(10, vol/20)`), so a captured track is
    self-contained exactly like the live path.

Wiring: have `SCut::ensureCapture()` (and the recursion) build/read these instead
of capturing the live `twTrackMix` (which internally pulls child rewires). Keep
it eagerly filled for now — every level is one resident buffer. This is correct
for arbitrary nesting because every child is read **random-access**.

Open Phase-1 details to settle in implementation:
- Where the recursion lives: a free helper `renderContainer(SObject&, env, rate)
  -> twRandomSource*`, or a new `twMixCaptureSource`, or a virtual
  `SObject::getCaptureSource()` (default = sample source; STrack/SStdMixer =
  recursive mix). A virtual on `SObject` reads cleanest and lets `twTrackMix`
  stay untouched.
- **Channels**: capture is mono today (`channels=1`); keep for the MVP.
- **Cycle guard already exists** at placement (proposal 05 §2.7), but the
  recursion should also defend (a container must not contain itself) to avoid
  infinite recursion if a future path lets it.
- **Invalidation**: reuse the existing `arrangementChanged → invalidateCapture`
  (coarse drop-all). A nested capture must drop when *any* descendant changes —
  drop-all already covers this.
- **Realtime safety**: same stance as today — (re)capture happens while stopped.

### Phase 2 — Demand paging

Fill blocks lazily on read instead of materialising the whole container up front.
A `twCachedSource : twRandomSource` wraps a *block producer* (`renderBlock(k,
dest)`), keeps an LRU/keep-all page map, and faults pages on `read()`. Matters for
long material and memory. `twCapturingSource` becomes the eager special case.

### Phase 3 — Content-addressed sharing

Identical producers (same cut window over the same container; same grain params;
same resample target) share **one** page set — proposal 06 §7 tier 3. Needs a
stable identity/hash for "the same render". Big win when an asset is placed many
times.

### Phase 4 — Finer invalidation

Dirty only the pages whose inputs changed instead of drop-all (e.g. an edit at
time *t* dirties pages covering *t* onward for the affected subtree). Pure
optimisation; correctness already holds with drop-all.

## 4. Relationship to existing code/proposals

- Subsumes the ad-hoc materialise-once sources; they can migrate onto the cache
  over time (no need to rewrite them for Phase 1).
- `06_GRAIN_PLAYBACK.md` §7 tier 3 (shared cache) is Phase 3 here.
- `07` (sample source/reader split, in STATE.md) established `twRandomSource` and
  `twCapturingSource` — the substrate this builds on.
- Playback is **not** changed by Phase 1 (it stays the streaming path); only the
  capture/preview path becomes recursive/random-access. Eventually the cache could
  back playback too, but that is out of scope here.

## 5. Acceptance (Phase 1)

1. A group track with **two sub-tracks holding different samples** previews as the
   **sum of both** (the bug in STATE.md).
2. Deeper nesting (group-of-groups) previews correctly.
3. Editing any descendant (move/add/mute) refreshes the asset preview.
4. Leaf-track and single-sample assets are unchanged (still correct).
5. No regression to playback.
