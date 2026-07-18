# Proposal 19: Fully-async, DAG-scheduled page freezing (fix shared-component freeze races)

> **Status: IN PROGRESS 2026-07-18.** Phase 0 (CWD-independent sample paths +
> `repeat_test.sh` gate) and Phase 1 (single-cursor freeze serialization) are
> committed. Phase 1b DIAGNOSED the flake precisely (issue #3b, see below) but
> its three targeted fixes all failed — so **Phase 2 is now the flake fix**, not
> just an architectural cleanup. Supersedes the ad-hoc concurrency of the current
> recursive `freezePage`. Motivated by a confirmed data race behind the flaky
> `takes_group_broadcast` test.
>
> Companion context (must-read before implementing):
> `plan/proposed/16_STALE_PAGE_FALLBACK.md` (stale-but-consistent playback),
> `plan/proposed/18_EXACT_POSITION_DOMAINS.md` (position math),
> `smaragd/docs/contracts/FREEZE_PROTOCOL.md`, `THREADING.md`, `CLIP_MODEL.md`,
> and the memory note `flaky_takes_group_broadcast`.

---

## Problem (confirmed, with evidence)

`smaragd/tests/cases/takes_group_broadcast.qxa` fails ~25–40 % of runs. On
failure, `group_comped.wav`'s should-be-silent tail `[96000,192000)` renders
stale pre-edit content (RMS ≈ **0.35274**, byte-identical every time), i.e. a
specific stale page slips through.

**It is a concurrency race** (proven 2026-07-18):
- `SMARAGD_REVAL_WORKERS=1` (see "Repro" — an env knob prototyped on
  `SProject`) → 16/16 pass. Default 8 workers → ~25 % fail.
- A freeze-collision tracker added to `twComponent::freezePage` (diagnostic,
  see "Repro") prints, on every failing run:

  ```
  [DBG FREEZE-COLLISION comp] type=twWavInput this=0xa8b377198 startPos=0 me=… other=… epoch=1
  [DBG FREEZE-COLLISION page] type=twWavInput this=0xa8b377198 startPos=0 me=… other=… epoch=1
  ```

  3–4 distinct threads freeze the **same `twWavInput`** (the single WAV source
  shared by both tracks, both takes, and the split head/tail — they all
  reference `test_sawtooth.wav`) at the same page, concurrently. `twWavInput`
  renders by advancing **one file cursor**, so concurrent freezes read each
  other's positions → cross-clip content corruption. Epoch stays 1 (a source
  never invalidates), so this is a pure instance-state race, not an epoch bug.

This is the general case: **a component shared by multiple graph paths is frozen
by multiple threads at once, and freezing mutates instance state.**

## Root cause (three coupled issues)

1. **Freezing mutates shared instance state.** `twComponent::freezePage_nolock`
   (`tw303a/graph/src/twcomponent.cc`) does `reset()/restoreInternalState()/
   seekTo()/seekInputStreams()` then `renderFrames()`; readers/sources advance
   `pos_`, DSP nodes advance filter/reverb memory. Two threads doing this to one
   component instance corrupt each other. The old shared-placeholder logic
   (twcomponent.cc, the `validAspects == 0` reuse branch) was crude, incorrect
   serialization — it actually let two threads render into the *same*
   `page->samples`.

2. **Sync recursion, many independent drivers.** `freezePage` is synchronous and
   recursive (`freezePage → renderFrames → calcOutputTo → readStreamingData →
   copyData → input->freezePage`). The readahead thread
   (`tw303a/playback/src/audio_engine.cc`), the offline render
   (`tw303a/render/src/render_session.cc`), and N `CaptureRevalidator` workers
   (`tw303a/schedule/src/capture_revalidator.cc`, default 8, set in
   `main/model/src/sproject.cpp`) each drive that recursion independently and
   collide on shared upstream components.

3. **Content-vs-epoch ordering windows.** Edits bump per-component content
   epochs (`twComponent::bumpContentEpoch`) but some content changes lag (lazy
   reader rebuild in `SCut::ensureReader`). A freeze that observes the new epoch
   but old content can stamp a stale page as current. (Secondary here — #1 is
   the dominant cause of *this* test — but must be closed for a complete fix.)

## Prior attempts (this session — all reverted, do not redo)

- **Private-page render + post-render epoch recheck** → *regressed* to 5/14: it
  parallelised same-component renders, amplifying issue #1.
- **Per-component `freezeMutex_` held across `freezePage`** → marginal (11/14):
  closes issue #1 for base-`twComponent` components, but a residual remains
  (issue #3 in the broadcast path) and the user judged an ambient mutex
  semantically muddy — the fix belongs in the ownership/scheduling model.

Committed this session and unaffected by this proposal (both good): twStreamingLatch
per-reader page hint (`f73ae14`), twTrackMix deferred shared_ptr drop (`744899a`).

---

## Goals

1. **No two threads ever render the same component concurrently** — by
   construction, not by an ambient lock.
2. **One call model: async.** No synchronous `freezePage`-calls-`freezePage`
   recursion; no synchronous pull from the playback/render threads.
3. **Correctly serve single-cursor components** (`twWavInput`, `twGrainSource`
   stream, sample readers, and future real VST plugins that expose exactly one
   processing instance): later requests queue behind the in-flight one.
4. **Exploit the DAG:** as freeze requests descend the graph they increasingly
   hit already-frozen, still-valid pages (leaves freeze first and cache).
5. Keep offline render bit-exact and keep proposal-16 stale-fallback playback.

## Non-goals / safety

- **Offline render stays exact.** It becomes a *consumer* of frozen pages
  (requests + waits), never a second concurrent freezer.
- **RT audio thread never allocates or freezes.** It reads ready pages and,
  when a page is pending, plays the stale-but-consistent predecessor
  (proposal 16). Unchanged guarantee.
- **Epoch/invalidation semantics from proposals 15/16 are preserved** — a page
  is served only if frozen AND `contentEpoch >= epochNow`.

---

## Design

### 1. Requests, not recursive calls

Replace the synchronous recursive call with a request that resolves later:

```
PageHandle requestPage(twComponent* c, uint64_t startPos, uint64_t epoch);
```

- If a frozen page with `contentEpoch >= epoch` is cached → resolve
  immediately (this is goal 4: descent hits cache).
- Otherwise enqueue a **freeze task** for `(c, startPos, epoch)` (deduped — see
  §5) and resolve the handle when it completes.

`PageHandle` is a completion handle (promise/future, or a callback + parked
continuation). Callers that must block (offline render) wait on it; callers
that must not (RT readahead) poll and fall back (proposal 16).

### 2. Render on readiness (invert the recursion)

A freeze task for `(C, pos)`:
1. Computes input dependencies: for each input port, the child component's
   page(s) covering `pos`'s time range (uses the existing position mapping —
   `twView::mapPos`, `twTrackMix` clip windows).
2. `requestPage`s each dependency. If all are ready → the task is **runnable**
   and renders `C` purely from those ready input pages. If any is pending → the
   task **parks** and is re-enqueued when its dependencies signal completion.

No thread holds a stack through the subgraph; the scheduler fills the DAG
bottom-up by readiness. The current pull (`calcOutputTo`/`copyData`) is replaced
by "read already-frozen input pages" — a pure function of inputs + carried
state.

### 3. Component execution policy — the single-cursor answer

Each component declares an execution class (new virtual, default parallel):

- **Parallel / pure** (gain, `twTrackMix`/`twMixer` summing *given* input pages,
  `twRewire` routing): freeze tasks run on any worker, parallel across
  positions — they mutate no shared instance state (their "state" is the input
  pages handed to them).
- **Serial-cursor / stateful** (`twWavInput`, sample readers, `twGrainSource`
  streaming, future VST): the component is an **actor** — it owns a
  single-consumer task queue. *All* its page tasks, from any track/take/thread,
  funnel into that one queue and run **one at a time, in timeline order**. A
  later request simply queues behind the current one.

  This is the clean form of "one cursor blocks later cursors": serialization by
  construction (no render-path mutex), and monotone cursor advance gives free
  state continuity. It is the principled version of the reverted `freezeMutex_`
  — a queue *owned by the component* instead of an ambient lock, which also
  gives **ordering** (in-position-order processing) that a mutex does not.

### 4. State is a cursor owned by its actor

For serial components, DSP/file state advances page-by-page on the actor's
queue (the user's cursor idea). Ordered requests need no snapshot restore. Only
out-of-order / seek requests trigger reset-and-fast-forward, or restore from a
snapshot page if the component supports it (existing `captureInternalState`/
`restoreInternalState` become the snapshot mechanism for random access).

### 5. Dedup + epoch consistency

- A `map<FreezeKey, TaskHandle>` where `FreezeKey = {component, startPos,
  epoch}` coalesces duplicate in-flight requests (the user's
  `map<twComponent*, JobEntry>` idea, correctly keyed per *page* now that tasks
  are per-page, not per-whole-graph recursion). Value holds a
  `shared_ptr<twComponent>` for lifetime.
- Because a serial component's edits and freezes are ordered through its actor
  (or stale-epoch queued tasks are dropped on a `bumpContentEpoch`), issue #3
  cannot produce "stale content stamped current". Pure components inherit
  correctness from their input pages' epochs.

### 6. Playback / render as pure consumers

- RT callback + readahead: `requestPage` ahead; consume ready pages; on pending,
  serve the stale predecessor (proposal 16, `audio_engine.cc:454-…`). No sync
  freeze on the audio path.
- `RenderSession`: `requestPage` sequentially and wait on each handle. Still
  sequential and exact; just no longer a *concurrent* freezer.

---

## Phasing (each phase independently shippable and test-gated)

### Phase 1 — Serialize single-cursor components (closes the confirmed freeze race)
Give `twWavInput`, sample readers (`twSampleReader`, and `twLoopReader` by
inheritance) a component-owned serial-cursor guard: `twComponent::freezePage`
holds `cursorMutex_` when `usesSerialCursor()` (default false; overridden true
on those). `twGrainSource` is a `twRandomSource` (read via `src_.read`, no
`freezePage`), so the reader that wraps it carries the cursor — no separate
change. Interim scaffolding; Phase 2's actor queue replaces it.
- **Acceptance (met 2026-07-18):** freeze-collision tracker prints **zero**
  collisions on any component (verified).
- **IMPORTANT finding:** eliminating freeze collisions did **NOT** fix the
  flake — `takes_group_broadcast` still fails ~40 % with the identical stale
  value (`0.35274`). So the freeze-collision was a *correlate*, not the cause.
  The flake's real cause is **issue #3** (content lags the content-epoch during
  the take-swap / edit-group broadcast: a background worker caches a page with
  pre-edit take-0 content stamped at the *current* epoch, and the sequential
  render accepts it). `workers=1` masks it by removing the second worker that
  caches the stale page during the edit window. **The flake fix is now
  Phase 1b** (below), not Phase 2 — see that section for why. Phase 1's value is
  only the invariant "single-cursor components are never frozen concurrently."

### Phase 1b — Pin & fix issue #3 (the actual flake) under range-scoped invalidation
Do this BEFORE the big Phase 2 inversion: it is small, high-value, and may make
the determinism gate green on its own; and it produces the precise spec of what
Phase 2 must guarantee. Starting Phase 2 blind to how issue #3 behaves risks
building against a moving target.

Context that changed the ground (must read first): proposal 18 Phase 5 landed
`invalidatePagesInRange_nolock` — trackmix edits now invalidate only the pages
in the edited *range* instead of bumping a coarse content epoch
(`smaragd/tw303a/mix/src/twtrackmix.cc`: `insertClip`/`removeClip`/`updateClip`
return a `twEditRange`; `SCut`/`STakeStack` edits flow through it). **Issue #3
now lives inside that machinery**, so it is likely a range-scoping or
edit/re-freeze ordering gap, not something that needs the full async rewrite.

What the 2026-07-18 provenance run established (evidence to build on):
- The stale tail is **take-0 content from a page frozen when the clip was the
  pre-split full `[0,192000)` take-0 clip** (trackmix logs showed
  `win=[0,192000) … childRMS≈0.376` feeding the tail page; after the split the
  tail clip resolves to a take-1 reader with `childRMS=0.0`).
- So after `split-clip` + `select-take 1` (broadcast to both tracks), a **cached
  page in the tail `[96000,192000)` from before the edit intermittently
  survives** and is served by the sequential render. `workers=1` avoids it;
  ≥2 workers hit it ~40 %.

Findings so far (2026-07-18, Phase 1b instrumentation, since reverted):
- **#3(a) RULED OUT — invalidation ranges are correct.** Logging `updateClip`'s
  united range and every `invalidatePagesInRange` showed the split's head resize
  computes `old=[0,192000) new=[0,96000) united=[0,192000)`, and the root
  `twMixer`/`twRewire` receive `[0,192000)`, `[0,96000)`, AND `[96000,192000)`
  invalidations. The vacated tail *is* invalidated. So it is NOT a range gap.
- **#3(b) CONFIRMED — a stale page is served at a CURRENT epoch.** On a failing
  run the render logs a cache-hit `type=twRewire pos=65536 epoch=22 rms=0.42`:
  a rewire tail page holding pre-edit (take-0, full-clip) content but stamped at
  the *current* content epoch, so `freezePage`'s fast path serves it. Since the
  ranges are correct, that page must have been (re)frozen with stale content and
  stamped current — i.e. content lagging its epoch, published by a concurrent
  freezer during the multi-step broadcast edit window.
- **Heisenbug caution:** always-on `fprintf` under the freeze path serialized it
  enough to make the flake vanish (6/6 pass). The next capture MUST be
  lightweight — e.g. a single one-shot dump triggered only when a tail page is
  served with `rms>0.01` (an atomic test-and-set), or a post-render assertion on
  the WAV — not always-on logging.

Update (2026-07-18, later): the exact publisher WAS pinned with a lightweight
one-shot (fires only on the stale serve, no Heisenbug): on every failure,
`[DBG STALE-SERVE] type=twRewire pos=65536 stampEpoch==liveEpoch serveTid==
stamperTid`. So **the render thread itself stamps AND serves the stale page, at
a consistent epoch** — not a worker stamping cross-thread, not an epoch bug. The
render's own recursive freeze produces stale content, and only under ≥2 workers.

**Fixes attempted for #3(b) — ALL FAILED (do not redo):**
1. private-page render + epoch recheck → regressed (5/14).
2. per-component freezeMutex_ (Phase 1) → 0 collisions but flake unchanged.
3. `SCut::getSnapshot(blocking)` on the freeze path (getRootComponent/seekTo/
   mapTimelineToComponentPos) — hypothesis was that the render read the stale
   `lastGoodSnapshot_` when a worker held the SCut mutex. **59/100, no change**
   → that was not the (main) path. Reverted.

**2026-07-18 root-cause finding (explains why #3 failed).** `SCut::currentReader_`
is a *struct of four `shared_ptr`s* that is **read and written under two different
locks**:
- read in `getSnapshot()` under `mutex()` (scut.cpp:43), plus an *unlocked* read
  in `rebuildReader()`'s fast path (scut.cpp:75);
- **written** (member-by-member reassignment + `generation++`) in
  `rebuildReader()` under `readerSwapLock_` — **not** `mutex()` (scut.cpp:153-167).
`readerSwapLock_` is used at *only* that one site; nothing reads under it. So the
swap was never atomic w.r.t. readers: a freeze on a render/worker thread can
observe a **torn** reader chain (new `.reader` + old `.grain`, or a `shared_ptr`
mid-reassignment → refcount hazard) while an edit swaps it. This is the "reads
mutable structural state a worker concurrently mutates" invariant, localized to
SCut reader management (not the twComponent page cache). It also explains why fix
#3 missed: blocking on `mutex()` never excluded a swap happening under
`readerSwapLock_`.

Targeted 2b fix (the two halves the prior attempts each did *separately*):
(1) unify all `currentReader_` access under `mutex()` — do the swap (and the
paired `builtXxx_` write) and the fast-path read under `mutex()`, retiring
`readerSwapLock_`; (2) resolve the freeze path with a **blocking** snapshot
(getRootComponent/seekTo/mapTimelineToComponentPos) so a freeze that observes the
post-edit epoch also observes the post-edit reader — never `lastGoodSnapshot_`.
The RT audio path keeps the try-lock → `lastGoodSnapshot_` fallback (proposal 16).
Gate is the confirmation; if <100/100, instrument the freeze-serve provenance.

Trigger in `takes_group_broadcast`: a *broadcast* places the **same `SCut`** on
two tracks, so both `twTrackMix` instances freeze that one SCut concurrently. On a
fresh process the reader is unbuilt (`readerTried_==false`), so both render
threads enter `rebuildReader()` at the same time — concurrent build + the
mismatched-lock read/write of `currentReader_` = a torn/garbage or stale tail,
timing-dependent (hence ~40-58% flaky). Also make `readerTried_` atomic.

**2026-07-18 CONFIRMED root cause (provenance capture, not hypothesis).** The
above two-lock fix + `getSnapshotBlocking` was implemented and did NOT fix the
gate (~50-70%). A decisive experiment settled the mechanism class: disabling ALL
background revalidation (`SMARAGD_NO_REVAL`, workers never run) → **15/15**;
workers on → ~50%. So the flake is the **offline render racing background
revalidation workers** — these are `<render>` actions, not playback. A
low-overhead in-memory provenance capture (no hot-path I/O → no Heisenbug),
dumped at exit and tagged per render, pinned the exact failure in the
`group_comped` render (3rd), tail region [96000,192000) which must be silent:

| run | `twSampleReader` asked at reader-pos | source content | tail RMS |
|-----|--------------------------------------|----------------|----------|
| PASS | 192000 / 227072 (past source end)   | none           | 0.0 (silence) ✓ |
| FAIL | 131072 (inside source)              | ramp           | 0.3769 ✗ |

The difference is exactly the split offset (96000): the tail split-take's source
start (`SCut::srcStart_` → `getStartOffset()`) is flakily **96000 (take1's raw
slip, pre-split-adjust)** instead of **192000 (slip + the 96000 of timeline the
head consumed)**. `mapTimelineToComponentPos` then addresses the shared
`twSampleReader` inside the sample instead of past its end, so the mixer/rewire
(which re-render fresh at the correct epoch) sum real audio into a region that
should be silent. So it is NOT a stale mixer cache and NOT the `currentReader_`
torn read alone — it is the split tail-take's source-offset / reader resolution
being left wrong by a background worker that touched the shared SCut between the
`split-clip`/`select-take` edit and the render. `readerTried_` then latches the
wrong build so the render cannot correct it.

Why the earlier partial fixes fell short:
- `currentReader_` two-lock unification + `getSnapshotBlocking` (KEEP — real UB
  fix) hardens the reader read, but the wrong value is `srcStart_`/the built
  reader itself, latched by `readerTried_`.
- Quiesce revalidation *during* the render (KEEP — correct, "offline renders stay
  exact") lifted ~50%→~70% but workers still corrupt the SCut in the window
  *between* the edit and the render's pause; `invalidateInputSubtree` (reverted)
  didn't help because the wrong state is the SCut reader/offset, not a latch-path
  page cache.

Candidate real fixes (decision pending):
  (A) Make the split tail-take's `srcStart_`/reader resolution correct and
      race-immune: recompute the source offset from current window params at
      resolve time (independent of any worker-built `currentReader_`), and reset
      `readerTried_` when the window/take changes so a worker's early build can't
      latch a stale offset.
  (B) The full request/ready inversion (the clean end-state): a freeze resolves
      structure from an immutable per-edit snapshot, never from live SCut state a
      worker mutates.

Three targeted fixes have now missed. Meta-conclusion: the incremental route is
not converging; the shared-state path the render reads and a worker corrupts is
subtler than each hypothesis. Two remaining options, in order of preference:
  (i) **Quiesce background revalidation for the duration of the offline render**
      (needs a pause/wait-idle on CaptureRevalidator; matches proposal 16's
      "offline renders stay exact"). Since `workers=1` all but eliminates it,
      removing workers entirely during the render should make it deterministic —
      and it is the smallest correct change if the stale page is stamped DURING
      the render (which STALE-SERVE shows: the render thread stamps it).
  (ii) Proceed to **Phase 2** (request/ready + actor) which orders freeze/edit by
       construction — the user's preferred clean end-state.

Step 1 — pin the exact publisher of the stale page (lightweight capture):
- Narrowed target: find WHO stamps the rewire/mixer tail page at the served
  epoch with non-silent content. Candidates: (b1) a revalidator worker freezes
  it reading epoch-after-bump but content-before the broadcast finished; (b2)
  the offline render races background revalidation that cached it during the
  edit window (RenderSession does not quiesce workers; contrast proposal 16's
  "offline renders stay exact"). Capture the stamping thread id + whether it is
  the render thread or a worker.
- Original (superseded) step 1 notes:
- Tag each render so logs are attributable (the `.qxa` runs 4 renders ×
  2 tracks × N pages; today's logs interleaved). Add a per-render sequence id
  (e.g. an env/counter bumped by the `render` action) into the trackmix and
  freeze logs.
- Log, for the tail pages of the failing (`group_comped`) render: which clip /
  component each clip resolves to, the child page's epoch, whether it was a
  fresh freeze or a cache hit, and which thread froze/stamped the served page.
- Determine which of these is true: (a) the split/select-take edit's invalidated
  range does not cover the pre-split clip's tail pages; (b) a background worker
  re-freezes a tail page *after* invalidation but reading pre-edit state
  (content-lags-invalidation ordering); (c) the broadcast to the second track
  applies the edit with a window where its clip/reader is transiently pre-edit.

Step 2 — targeted fix, depending on the finding:
- (a) → widen the invalidated range (or invalidate the union of pre/post-edit
  extents) so no pre-edit tail page survives; likely in the `twEditRange`
  computation for split/select-take.
- (b) → make edit → invalidate → re-freeze ordered so a re-freeze cannot read
  pre-edit state (e.g. invalidate under the same lock/sequence that applies the
  model change; or stamp re-freezes against a monotonically-checked edit id).
- (c) → make the edit-group broadcast apply model change + invalidation
  atomically per member before any freeze can observe an intermediate state.
- **Acceptance:** `repeat_test.sh takes_group_broadcast 100` → 100/100 at the
  default worker count, and swept over `SMARAGD_REVAL_WORKERS` ∈ {1,2,4,8,16};
  offline-render goldens bit-identical; full qxa suite green.

If Phase 1b cannot make it deterministic without the recursion inversion, it
still yields the exact invariant Phase 2 must enforce, and we proceed to Phase 2
with that spec.

### Phase 2 — Invert the mix graph to request/ready scheduling (THIS is the flake fix)

**Why this fixes issue #3(b) when the targeted fixes could not.** Phase 1b
pinned the flake to the render thread's *own recursive freeze* producing stale
content: `freezePage_nolock` synchronously pulls its inputs
(`freezePage → renderFrames → calcOutputTo → copyData → input->freezePage`) and,
along the way, reads mutable component/clip state (readers, clip windows, active
take, capture) that a background revalidation worker is concurrently mutating.
No single lock closes this because the read set spans the whole subgraph and many
objects. The request/ready model removes the race *by construction*: a freeze
task renders **purely from already-frozen, immutable input pages** it requested
and received — it never pulls live, mutating state mid-render. Combined with the
per-component serial actor (Phase 1's `usesSerialCursor`, promoted to a real
queue here), a component's edits and freezes are ordered, so content can never
lag its epoch. That is the invariant the three incremental fixes each only
partially enforced.

**Scope.** Convert the cached, pulling components — `twTrackMix`, `twMixer`,
`twRewire`, `twPluginChain`/`twPluginInsert` — to render from ready input pages
instead of the synchronous `copyData` pull. Introduce the request/ready
scheduler and the per-`(component,startPos,epoch)` dedup map (the user's
`map<twComponent*, JobEntry>`, correctly keyed per page).

**Sub-steps (each independently buildable + gated):**
- **2a — `requestPage(component,startPos,epoch) → PageHandle`** over the existing
  `outputPages_` cache: cache-hit (frozen + current) resolves immediately; else
  enqueue one freeze task (deduped) and resolve on completion. No behaviour
  change yet — just the handle API + dedup map.
- **2b — readiness-driven freeze** in `freezePage_nolock`: a task computes its
  input page dependencies, `requestPage`s them, and renders **only from the
  returned ready pages** (replace `calcOutputTo`/`copyData` pulls with reads of
  ready input pages). Park + re-enqueue if a dependency is pending. This is the
  core inversion and where the flake dies — verify against the N=100 gate here.
- **2c — retire the interim guards** now subsumed: Phase 1's `cursorMutex_`/
  `usesSerialCursor` becomes the actor's serial queue; drop the `getSnapshot`
  stale-fallback risk on the freeze path (freeze no longer reads live SCut state,
  only frozen pages).

**Watch-outs (from Phase 1b):**
- The graph is an acyclic DAG guarded by `FreezeContext`; keep that guarantee so
  the scheduler's readiness walk is ancestor-after-descendant (no cycle → no
  deadlock/livelock).
- `twTrackMix` already has its own (non-caching) `freezePage` that holds
  `mutex()` across recursion — fold it into the same model.
- Preserve proposal 16 stale-fallback playback and proposal 18 Phase 5
  range-scoped invalidation semantics.

- **Acceptance:** `repeat_test.sh takes_group_broadcast 100` → **100/100** at the
  default worker count AND swept over `SMARAGD_REVAL_WORKERS ∈ {1,2,4,8,16}`
  (this is the whole point); no `freezePage`→`freezePage` *synchronous* recursion
  remains (assert via a stack-depth guard); full qxa suite green; offline-render
  goldens bit-identical to the Phase 0 references; no perf regression on the
  render benchmark (§ Test plan).

### Phase 3 — Async readahead / render
`AudioEngine` readahead and `RenderSession` become request-driven consumers.
Remove the last synchronous pulls.
- **Acceptance:** RT thread never calls `freezePage` (guard/assert); playback
  underrun + stale-fallback tests (proposal 16) still pass; offline render
  bit-identical to Phase 0 golden WAVs.

---

## Test plan (built from the start — do NOT defer)

**Reproduction harness (Phase 0, land first).** Make the flake a *deterministic
gate* so every later phase is measurable:
1. Fix the test foot-gun: the `.qxa` uses relative `../test_sawtooth.wav`, which
   silently resolves to the stale 1048576-frame `qbx/test_sawtooth.wav` when run
   from the wrong CWD. Either resolve sample paths relative to the `.qxa` file,
   or delete the stale root file. (See memory `flaky_takes_group_broadcast`.)
2. Add a repeat-runner: run `takes_group_broadcast` (and the grain/render suite)
   **N=100** times at the default worker count; require 100/100. One iteration
   ≈ a few seconds, so N=100 is a CI-scale gate.
   Command (correct CWD + output dir mandatory):
   ```
   cd smaragd/tests/cases
   BIN=../../build/bin/smaragd.app/Contents/MacOS/smaragd
   for i in $(seq 100); do "$BIN" --test-case ./takes_group_broadcast.qxa \
       --test-output-dir /tmp/o 2>/dev/null | grep -q '^PASS - ' || echo "FAIL $i"; done
   ```

**Unit tests (new, `tw303a/graph/tests/` + `schedule/tests/`):**
- *Serial-actor ordering:* a stateful stub component records the order/positions
  it was asked to render; fire 50 concurrent `requestPage`s across positions
  from 8 threads; assert it processed them **one at a time** and its cursor
  advanced monotonically (no interleave).
- *Shared-source correctness:* one source, two independent reader chains request
  overlapping pages concurrently; assert both get byte-correct data (the direct
  `twWavInput` race regression test).
- *Dedup:* two identical `(component,pos,epoch)` requests in flight → exactly one
  freeze task runs; both handles resolve to the same page.
- *DAG readiness:* a 3-level graph; assert a parent task renders only after all
  child pages are ready, and a re-request of an already-frozen child is a cache
  hit (no re-render — count renders via a stub).
- *Epoch/edit interleave:* bump a component's epoch mid-freeze; assert no page
  with old content is ever served as `contentEpoch >= epochNow`.

**Stress test (new):** randomized edits (select-take/split/slip broadcast) +
concurrent playback + revalidation on a multi-track/multi-take project for a
fixed wall-clock budget, with the freeze-collision tracker compiled in as an
assert (abort on any collision). Run under TSan if feasible on macOS/Linux.

**Regression gates (must stay green each phase):**
- Full qxa suite from `smaragd/tests/cases/` (grain, render_sawtooth, takes_*),
  run correctly (right CWD + `--test-output-dir`).
- Module tests: `graph`, `mix`, `sources`, `plugins`, `playback`, `render`,
  `schedule`.
- Offline-render golden WAVs: freeze a set of reference renders at Phase 0;
  assert bit-identical after each phase (async must not change output).
- `python tools/check_layering.py` clean.

**Determinism check:** the reproduction harness (N=100) is the headline gate for
Phases 1–3; also run with `SMARAGD_REVAL_WORKERS` swept over {1,2,4,8,16}.

---

## Key files & symbols (implementer map)

- Freeze core: `tw303a/graph/src/twcomponent.cc`
  (`freezePage`, `freezePage_nolock`, `getOrAllocatePage`, `outputPages_`,
  `contentEpochNow`/`bumpContentEpoch`), header
  `tw303a/graph/include/tw/graph/twcomponent.h`.
- Serial-cursor sources: `tw303a/sources/src/twwavinput.cc`,
  `twsamplereader.cc`, `twgrainsource.cc`, `twloopreader.cc`.
- Mix graph: `tw303a/mix/src/{twtrackmix,twmixer,twrewire}.cc`,
  `tw303a/plugins/src/{twpluginchain,twplugininsert}.cc`.
- Pull seam being replaced: `tw303a/graph/src/twlatch.cc` +
  `twstreaminglatch.cc` (`copyData`/`readStreamingData`).
- Schedulers/drivers: `tw303a/schedule/src/capture_revalidator.cc`
  (worker pool; `numWorkers` from `main/model/src/sproject.cpp`),
  `tw303a/playback/src/audio_engine.cc` (readahead),
  `tw303a/render/src/render_session.cc` (offline render).
- Page type + fallback field: `tw303a/pages/include/tw/pages/tw_output_page.h`
  (`contentEpoch`, `validAspects`, `stalePredecessor`, `pageMutex`,
  `internalState`).
- Cycle guard (keep — prevents self-recursion): `tw_freeze_context.h`.

**Diagnostic instrumentation** currently in `twcomponent.cc` (flagged
`[DBG freeze-collision]`, uncommitted): a global tracker that prints when two
threads render the same component/page. Keep it while implementing Phases 1–2
(promote to an assert in the stress test); strip before final merge.

## Effort / risk

Large, staged. Phase 1 is small and high-value (kills the confirmed race).
Phase 2 is the real architectural work (recursion inversion + scheduler) and the
main risk surface (deadlock/liveness, back-pressure, latency). Phase 3 is
mechanical once 2 lands. Mitigations: each phase gated by the N=100 repro + the
bit-identical render golden + layering check; the collision tracker as a
compiled-in assert during development.

## Open questions

1. **Deadlock/liveness of the task scheduler** under diamond DAGs and
   serial-actor back-pressure — needs an explicit ordering argument (the graph
   is an acyclic DAG guarded by `FreezeContext`; formalize that acquisition is
   always ancestor-after-descendant in the request/ready model).
2. **Back-pressure & memory:** unbounded parked tasks vs the fixed
   `CapturePagePool` (2048 pages). Cap in-flight requests; define eviction.
3. **Preview vs playback vs export aspects:** do they share one task graph with
   priorities (current `Job.priority`), or separate lanes? Prefer one graph,
   priority-ordered.
4. **VST reality:** a plugin with N instances = N parallel cursors; model as a
   small actor pool per component rather than strict serial. Design the actor
   abstraction to take a concurrency-degree (1 for `twWavInput`, N for a
   pooled plugin).
5. Whether to keep `stalePredecessor` or rely on "old page stays cached until
   atomically replaced" (the request model makes the latter natural).
