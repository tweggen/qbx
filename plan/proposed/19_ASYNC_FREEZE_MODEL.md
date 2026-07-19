# Proposal 19: Fully-async, DAG-scheduled page freezing (fix shared-component freeze races)

> **Status: FLAKE FIXED 2026-07-19; inversion is now optional cleanup.**
> The `takes_group_broadcast` flake — the thing that motivated this proposal — was
> ROOT-CAUSED and FIXED in the EDIT path (commits `a1e6011` + `48a38bd`), NOT via
> the freeze-side inversion. See "ACTUAL ROOT CAUSE + FIX" below: a stale
> try-lock `SCut::getDuration()` in `STakeStack`'s `durationChanged` emits let a
> split clip's head stay full-length under worker contention. Gate is now
> **100/100 deterministic** (swept over `SMARAGD_REVAL_WORKERS ∈ {1,4,8,16}`).
>
> What landed and stays (all correct on their own merits, none required for the
> flake but good hardening): Phase 0 (CWD-independent sample paths +
> `repeat_test.sh` gate), Phase 1 (single-cursor `cursorMutex_` freeze
> serialization), Phase 2a (`requestPage` dedup front door, `8621a3b`), and the
> reader/render hardening (`72a63e6`: `SCut::currentReader_` unified under
> `mutex()` retiring `readerSwapLock_` — a real UB fix; `getSnapshotBlocking()`;
> `readerTried_` atomic; `CaptureRevalidator::pause()/resume()` drain wired via
> `SProject`; `RenderSession` runs `onComplete` before clearing `running_`).
>
> **The request/ready inversion (Phase 2b) is now a FORWARD-LOOKING architectural
> cleanup, not a bug fix** — see "The request/ready inversion" section. It removes
> a whole class of shared-component freeze races by construction and lets the
> interim guards retire, but it is optional and independently gated. The
> investigation history below is kept verbatim for context (it contains several
> superseded hypotheses — read the dated "CONFIRMED"/"ACTUAL ROOT CAUSE" notes as
> the ground truth).
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

> **REFINED (2026-07-19):** see "Component execution classes" in Phase 2
> REVISED — the two classes below become the concurrency-degree knob
> `∞ | N | 1 | 0` (pure / poolable / exclusive lane with runs+pre-roll /
> real-time-bound capture-only), and the serial actor becomes the lane +
> predecessor-edge form.

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

Correction (2026-07-18, after reading the source): `acquireReader` mints a
**fresh** reader per SCut (twsamplereader.cc:14), so this is NOT a shared-reader
`startOffset` fight. The real inconsistency is in `SCut::getRootComponent()`: it
returns the cut's OWN built reader iff `readerTried_`, else falls back to
`content_->getRootComponent()` (the content's *shared* source component). A
background worker building the tail cut's reader between the edit and the render
flips which branch the render takes, and the position mapping (`srcStart` folded
for the built reader vs the shared source's own domain) becomes inconsistent →
the shared `twSampleReader` is addressed at 131072 (inside the sample) instead of
227072 (past its end). So the flaky quantity is *which component + mapping* the
tail resolves to, decided by a worker-vs-render race on the SCut's lazy reader
build. The provenance table above is the ground truth; this is the mechanism.

**DECISION (user, 2026-07-18): pursue (B), the request/ready inversion** — the
clean end-state. (A) — race-immune lazy resolution + `readerTried_` reset — was
the smaller alternative; not chosen.

**ACTUAL ROOT CAUSE + FIX (2026-07-18, render-tagged provenance — supersedes the
reader-resolution theory above).** Deeper capture proved the flake is NOT on the
freeze/reader-resolution path at all — it is an **edit-path stale read**, so
neither (A) nor (B) as framed would fix it. Render-tagged twTrackMix mix capture
for the failing `group_comped` render showed one track carrying TWO tail
contributions: the correct tail clip (`startTime 96000, dur 96000`, silence) AND
a stale **head clip at `startTime 0, dur 192000`** (not shortened) bleeding take0
(rms 0.3769) into [96000,192000). The `updateClip` trace then showed the split's
head-shorten `updateClip(head, …)` firing with `newDur=192000` (not 96000) for
the flaky track. Mechanism: `STakeStack::setDurationAll` (the split's
head-shorten) emitted `durationChanged(getDuration())`; `getDuration()` →
`activeCut()->getDuration()` → `SCut::getSnapshot()` which uses a **try-lock →
stale `lastGoodSnapshot_`** fallback. When a background revalidation worker held
the SCut mutex, `getDuration()` returned the pre-shorten 192000, which propagated
`STrack::trackChildDurationChanged → twTrackMix::updateClip(head, 192000)`,
leaving the head full-length. `SMARAGD_NO_REVAL` (no worker ever holds the mutex)
→ 15/15; workers on → ~50%.

**FIX (committed `a1e6011`):** `STakeStack::setDurationAll` now emits
`durationChanged(duration)` — the authoritative value it just set on every take —
instead of re-reading via the try-lock `getDuration()`. Race-free. Gate: **N=100
→ 100/100 deterministic** at default workers, and 40/40 each swept over
`SMARAGD_REVAL_WORKERS ∈ {1,4,16}`. This is an EDIT-path fix; the freeze-side
hardening (72a63e6) is correct but was not what fixed the flake. The request/ready
inversion (below) remains a valid future cleanup but is NOT required.

**Sibling emits hardened (committed `48a38bd`):** the other STakeStack emitters
(`setActiveTake`, `removeTake`, `onTakeCutChanged`, `applyWindowAll`) shared the
same stale try-lock `getDuration()` shape and could flake other broadcast edits.
Added `SCut::getDurationBlocking()`/`STakeStack::getDurationBlocking()` (blocking
snapshot, edit-path only — no `getDuration()` caller runs on the RT audio thread)
and routed those emits through it; `applyWindowAll` emits its authoritative
`duration` like `setDurationAll`.

### The request/ready inversion — forward-looking design (current state)

**This is now optional.** The flake that drove it is fixed (edit path). But the
underlying structural weakness the inversion targets is still real and worth
removing when the app pushes further into concurrent freezing (real-time
playback + preview + export overlapping): **freezing a component MUTATES its
instance state** (`reset`/`seekTo`/`restoreInternalState` → cursor + DSP memory),
and a component shared by multiple graph paths (one `twWavInput` under both
tracks, both takes, split head/tail) can be frozen by several threads at once.
Today that is held together by Phase 1's per-component `cursorMutex_` (one freeze
at a time per component) plus the freeze-side hardening — correct but coarse.

**Target invariant.** A freeze resolves ALL structure — which component, which
reader, the position mapping — from a single immutable snapshot taken atomically
with the content epoch, and renders only from already-frozen input pages, never
from live SCut/reader/clip state a worker can mutate. With that, the shared
mutable cursor stops being observable across threads and the interim guards go
away.

**Already landed toward it (reuse, don't redo):**
- `requestPage(component,startPos,epoch)` dedup front door over `outputPages_`
  (Phase 2a, `8621a3b`) — the request half of request/ready; drivers
  (revalidator, offline render, playback readahead) already call it.
- `SCut::currentReader_` published + read under one lock; `getSnapshotBlocking()`
  for consistent structural reads; `readerTried_` atomic (`72a63e6`).
- `CaptureRevalidator::pause()/resume()` with in-flight drain (`72a63e6`) — lets
  an exclusive consumer (offline render) own the graph; a stepping stone toward
  a per-component actor.

**Remaining sub-steps (each independently buildable + gated; NOT flake-blocking):**
- **Inv-1 — LANDED 2026-07-19** (see STATE.md entry of that date): `twResolvedClip`
  + `twView::ResolveFn` replace the `mapPos()`/`getComponent()` pair;
  `SCut::resolveClip` resolves under ONE `getSnapshotBlocking()`;
  `STakeStack::resolveClip` reads `activeCut()` once. Same session also landed
  `CaptureRevalidator::retireObject` (SCut-destruction UAF crash fix, with
  `schedule_test`) and the `getDurationBlocking()` insert-path fix
  (takes_recording_placement doubling — the a1e6011 stale try-lock class at the
  STrack insert/move sites). CAUTION: the `SMARAGD_REVAL_WORKERS`/
  `SMARAGD_NO_REVAL` env knobs no longer exist in the source (instrumentation
  prototypes, since removed) — `repeat_test.sh`'s workers argument is currently
  a no-op; re-add the knob before the next sweep-gated phase. Original spec:
- **Inv-1 (spec) — one structural-resolution snapshot per freeze.** `twView` currently
  calls `mapPos()` (→ `SCut::mapTimelineToComponentPos`, which lazily builds the
  reader) and then `getComponent()` (→ `SCut::getRootComponent`, which returns
  the built reader iff `readerTried_` else the shared content component) as two
  separate reads that can straddle a concurrent lazy build. Replace them with ONE
  `SCut`/`STakeStack` resolution — `{ component, mappedPos }` (plus `srcStart`,
  `looping`) — computed under `mutex()` and captured once per `twView` freeze, so
  component and mapping can never disagree and the result never depends on
  `readerTried_` timing. (This is the `twView` two-callback → single-resolver
  change: `twView` ctor, `twTrackMix::insertClip`, `STrack`, `SObject`/`SCut`/
  `STakeStack`.)
- **Inv-2 — freeze from ready pages only.** Convert the caching mix-graph nodes
  (`twMixer`/`twRewire`/`twPluginChain`/`twPluginInsert`, and fold in
  `twTrackMix`'s own freeze) to request already-frozen input pages via
  `requestPage` and render only from them, replacing the synchronous
  `calcOutputTo`/`copyData` live pull. Park + re-enqueue when a dependency is
  still pending. Keep the acyclic DAG guard (`FreezeContext`) so the readiness
  walk stays ancestor-after-descendant (no deadlock/livelock).
- **Inv-3 — retire the interim guards** now subsumed: promote `cursorMutex_`/
  `usesSerialCursor` into the per-component actor's serial queue; the
  `getSnapshotBlocking()` freeze-path reads and the render-quiesce `pause()`
  become unnecessary once a freeze never reads live structural state (keep only
  as belt-and-suspenders if a gate regresses without them). The edit-path
  `getDurationBlocking()` emits (the actual flake fix) stay regardless — they are
  independent of the freeze model.

**Acceptance for each sub-step:** `repeat_test.sh takes_group_broadcast 100` →
stays 100/100 across `SMARAGD_REVAL_WORKERS ∈ {1,2,4,8,16}`; offline-render
golden WAVs bit-identical to the current output; full qxa **audio** asserts
green; `python tools/check_layering.py` clean. Because the flake is already gone,
these gates are regression gates, not the pass/fail of the fix.

**Design review of Inv-2/Inv-3 (2026-07-19, after Inv-1 + the session's two
bug fixes — amends the spec above):**
- *Clip-window snapshot is the missing half of Inv-2.* `twTrackMix::freezePage`
  today holds `mutex()` across the ENTIRE child recursion, which is what makes
  the clip list consistent during a freeze. Unwinding the recursion removes that
  accidental consistency: an Inv-2 freeze task must capture, at creation under
  `mutex()`, ONE atomic snapshot of {overlapping clip windows, each clip's
  `resolveClip` result, epoch} — the trackmix-level analogue of Inv-1. Without
  it Inv-2 re-opens the edit/freeze race one level up.
- *Parked tasks are a lifetime surface.* The 2026-07-19 SCut UAF crash (reval
  queue's borrowed pointer + one-way `deleteLater`) is the proof-of-class.
  Invariant: every task/dedup/continuation reference is owning (`shared_ptr`)
  or covered by a destructor-time `retireObject`-style drain; unit-test the
  "destroy component with parked dependents" path like `schedule_test` does.
- *Parking must not occupy a worker* (else diamond fan-in deadlocks the pool at
  pool width), and a task may not hold its component's `mutex()` while
  requesting dependencies — the inversion must actually unwind the under-lock
  recursion, not wrap it.
- *Every caching node is serial-in-position* — `clip.previousPage` /
  `page->internalState` chaining forces in-order page processing per node.
  The actor model applies to mix nodes too; parallelism is across components
  only, never within one.
- *Open question 3 (preview vs playback lanes) is REQUIRED for Inv-2, not
  optional:* `freezePreviewPage` restores/captures the SAME `internalState`
  chain as full-rate freezes (twcomponent.cc:694/727), so interleaved aspects
  on one actor corrupt each other's state chain. Split state per aspect, or
  restore-from-snapshot on every cross-aspect switch.
- *Dedup key vs range invalidation:* `{component,startPos,epoch}` assumes a
  monotonic epoch, but prop-18 P5 edits invalidate page RANGES. A parked task
  must re-verify all input pages are still valid atomically with publishing
  (or invalidation must propagate to parked dependents) — else it can publish
  stale-content-at-current-epoch, the exact #3(b) shape.
- *Inv-3: retire by assert first.* Convert `cursorMutex_` to a
  try_lock-must-succeed assert for a full gate cycle before deleting; keep the
  `pause()` Guard as a no-op shim one phase longer. And the edit-path
  `getDurationBlocking()` reads are PERMANENT, not interim — the class
  re-occurred 2026-07-19 at the STrack insert sites (takes_recording_placement
  doubling); Inv-2/3 restructure only the freeze side.
- *Do BEFORE Inv-2 (more robustness per line):* (1) re-add the
  `SMARAGD_REVAL_WORKERS` knob — it no longer exists, the sweep argument of
  `repeat_test.sh` is a silent no-op, so the acceptance sweeps above cannot
  currently run; (2) kill the fresh-cut default-snapshot fallback (initialize
  `lastGoodSnapshot_` from ctor state, or block the first-ever `getSnapshot()`)
  so the stale try-lock class cannot return duration-0 at all; (3) audit the
  remaining edit-path `getDuration()`/`getSnapshot()` callers (e.g. the
  `place-recording` column scan, splacements helpers).
- *Staging:* 2i trackmix task-input snapshot (small, independently gated) →
  2ii mixer/rewire/chain request+ready with park mechanics → 2iii offline
  render as request+wait consumer → Inv-3 assert-then-delete.
  **SUPERSEDED same day by the dataflow design below** (park/re-enqueue is
  dropped entirely; stage 2i survives as the planner's structural snapshot).

### Phase 2 REVISED (2026-07-19, user-approved): demand-driven dataflow — async pages only

**Decision.** Drop the park/re-enqueue request/ready sketch. The goal (user):
remove the *mixture* of synchronous recursive `freezePage()` and async frozen
pages — async pages only, nothing inside the graph ever awaits a result.
Parking is still synchronous thinking deferred (a task requests, suspends,
resumes — carrying a continuation-lifetime surface just to simulate the old
recursion). The clean model is a **dependency-counting dataflow scheduler** —
"Ninja for pages":

**Model.**
- **Node = `(component, pageIndex, epoch)`** — the unit of work is "produce
  this one page". Dependency edges are ordinary and uniform:
  (a) the **input pages** its window overlaps, and (b) the **predecessor page**
  `(component, pageIndex-1)`. The `previousPage`/`internalState` state chain
  stops being a special actor concept — it is just another DAG edge, and
  in-position order per component falls out of the graph.
- **Nothing waits.** A node is either *ready* (all edges satisfied → priority
  ready-queue) or pending counters. Workers pull ready nodes and run a
  **non-recursive leaf renderer**: `freezePage_nolock` refactored to take an
  explicit set of already-frozen input pages instead of pulling latches live.
  Completion decrements dependents' counters; zero → ready-queue. No parked
  continuations, no blocked workers → diamond-DAG deadlock impossible **by
  construction**.
- **Consumers never call freezePage — they declare demand.** Each consumer
  owns a *watermark*: "root pages covering [t, t+horizon) at epoch E".
  Playback readahead advances it with the playhead (RT keeps prop-16 stale
  fallback); the **offline render** advances page-by-page and waits on a CV
  for "root page k valid" — the only blocking in the system, at the very edge,
  outside the graph (bit-exactness free: the render observes, never executes);
  preview is the same at low priority.
- **Planner.** A watermark expands into nodes by a *structural* walk (no
  rendering): under each component's `mutex()` once, capture an immutable
  per-node input plan — clip windows + `resolveClip` results (Inv-1) + epoch.
  Renders never touch live model state; they read plans and pages. (This is
  the design-review's "2i" snapshot, kept.)
- **Edits.** Epoch/range invalidation as today (prop 18 P5). Stale nodes are
  dropped at two checkpoints: on dequeue, and **verify-at-publish** (all input
  pages still valid, atomically with the cache insert). No invalidation
  propagation through parked tasks — none exist.
- **Lifetime.** Nodes own `shared_ptr` to their component; component
  destruction drops its planned/ready nodes and drains in-execution ones via
  the `retireObject` mechanism (landed 88bb896).
- **Back-pressure.** The watermark horizon bounds outstanding work (readahead
  N pages, render 1-2, preview coarse) — answers open question 2 without an
  eviction policy for in-flight work.

**Component execution classes (resolves §3 and open question 4).** Each
component declares `concurrency = ∞ | N | 1 | 0`:
- **∞ — pure** (gain, rewire, mixing *given* input pages): stateless, NO
  predecessor edge at all — parallel across positions.
- **resumable stateful** (readers, grain, own DSP): predecessor edge; a broken
  chain (seek, invalidation) repairs by reset+fast-forward or
  `captureInternalState`/`restoreInternalState` (today's machinery).
- **N — poolable plugin**: N independent *lanes*, each its own instance +
  state. Preview gets its own instance from the pool → the preview-corrupts-
  playback state-chain hazard disappears structurally for anything poolable
  (resolves open question 3 for this class).
- **1 — exclusive instance** (real VST): ONE physical cursor, state not
  faithfully restorable (getState/setState is not sample-accurate for tails).
  The planner emits **runs** (maximal contiguous demanded intervals), not
  independent nodes; the component owns a capacity-1 **lane**; a node is ready
  when (inputs frozen) AND (head of its lane's current run). Repositioning
  between runs is an explicit protocol: reset (suspend/resume, all-notes-off)
  + **pre-roll** (render-and-discard K frames, K = declared tail length).
  Determinism comes from the protocol, not state capture: same inputs + reset
  + same pre-roll ⇒ same output — and the offline render demands pages
  sequentially from zero = ONE run, the plugin's native streaming case, zero
  repositions. Scheduler is thrash-averse: finish the current run to its
  demand horizon before switching; priority decides lane contention
  (playback > preview; preview serves stale/coarse meanwhile).
- **0 — real-time-bound** (physical hardware in the chain): cannot render
  faster than real time, cannot rewind. No lane; its pages come ONLY from live
  capture (playback / real-time bounce — the industry answer for external
  gear), mapping onto the existing `twCapturingSource`/capture machinery. The
  planner treats them as external inputs: demand cannot force production;
  downstream goes prop-16 stale fallback until a capture pass exists.
- **Freeze-in-place escape hatch:** chronic contention on a capacity-1/0
  component → bounce its output to pages once and treat as pure until inputs
  change; "frozen pages standing in for a component" is the system's native
  currency, so this is nearly free.

**Why this beats park/re-enqueue:** no parked-task lifetime surface (tasks
exist only when runnable); deadlock-free by construction instead of by
argument; serial-cursor sources need no special call model (predecessor edge /
lane); the render becomes an observer, making bit-exactness trivial; the sync
recursion is deleted outright rather than simulated.

**Migration (each stage independently gated on the N=100 gate + bit-identical
goldens + module tests + layering):**
1. Extract the leaf renderer (explicit-inputs `freezePage_nolock` variant);
   sync path temporarily feeds it — no behaviour change.
   **DONE 2026-07-19** (see STATE.md "dataflow stage 1"): `twFrozenInputs` +
   `twFrozenInputScope` (tw/graph/tw_frozen_inputs.h),
   `twComponent::freezePageFromInputs`, and the seam in
   `twStreamingLatch::copyData` (bound page → no recursion; miss → recorded +
   legacy pull). mix_test seam suite proves bypass, byte-identity, and
   fallback.
2. Planner + per-node structural snapshot (reuses Inv-1 `resolveClip`).
   **DONE 2026-07-19** (see STATE.md "dataflow stage 2"): `twPagePlan`/
   `twComponent::planPage` (base: per-input-plug grid deps; `twTrackMix`
   override: per-clip `twView::resolve` — the same Inv-1 resolution the
   render uses), `freezePageWithInputs` (virtual-path planned render), and
   the bound-serve seam extended to the top of `twComponent::freezePage`
   (self-skip + miss recording) so the trackmix's DIRECT child-freeze path
   is covered alongside stage 1's copyData seam. mix_test proves plan
   correctness and byte-identical plan-driven rendering.
3. Dependency-counting scheduler inside `CaptureRevalidator` (it already has
   the pool, priority queues, pause/drain, retireObject).
   **DONE 2026-07-19** (see STATE.md "dataflow stage 3"): `GraphDemand`/
   `requestGraphPages` (the watermark), `PageNode` counters with owning refs,
   predecessor edges as ordinary counter edges (in-position order + state
   chaining, no actor machinery), verify-at-publish with one bounded retry,
   three-way priority in workerLoop, pause/drain + shutdown-abort integration.
   schedule_test proves dependency ordering, exact render counts, cache-hit
   dedup, pause gating, and epoch-staled re-render.
4. Offline render → watermark consumer (bit-identical goldens gate).
   **DONE 2026-07-19** (see STATE.md "dataflow stage 4"): `RenderSession::
   setScheduler` + sequential per-page demands (full-range look-ahead
   deliberately reverted: it re-rendered the NON-CACHING twTrackMix out of
   position order against the in-order chain — pipelining returns once node
   results are the cache for such components);
   `pauseBackground()/resumeBackground()` background-only quiesce (full
   pause would deadlock the demand waits; none at all was nondeterministic).
   Gate: 11 golden WAVs bit-identical to the pre-change baseline AND
   deterministic across 3 runs.
5. Readahead → watermark consumer; RT unchanged.
   **DONE 2026-07-19** (see STATE.md "dataflow stage 5"):
   `AudioEngine::setScheduler` — the readahead demands the remaining
   not-yet-current window (never blocks, never renders, never re-demands
   valid pages) and observes on its 20ms tick; RT path untouched;
   `twSpeaker::setPageScheduler` pass-through wired per project. Goldens
   stayed 11/11 bit-identical. Interactive playback pending a by-ear check.
6. Delete the sync recursion; `cursorMutex_` → must-succeed assert for one
   full gate cycle, then removed (Inv-3). Edit-path `getDurationBlocking()`
   reads stay permanently.

**Prerequisites — DONE 2026-07-19** (see STATE.md entry "Phase 2
prerequisites"): `SMARAGD_REVAL_WORKERS` re-added (0 = no revalidator; sweep
is real again, verified by thread count), fresh-cut default-snapshot fallback
eliminated (ctor init + refresh-on-mutation of `lastGoodSnapshot_` — try-lock
failure now serves at worst a one-edit-stale REAL window), edit-path
stale-read audit converted to blocking (split/resize/duplicate/add-take/
place-recording/remove-asset actions; `buildCapture_`; `mapChildRangesToSelf`).
Deliberately left try-lock: getTopMostSLinkAt, child sort comparator,
getChildrenExtent, `STakeStack::getDuration` itself, `sstdmixerview.cpp`
gesture handlers (audit when that file settles). Gate: 50/50 at each of
workers {1,4,8,16}. Next: migration stage 1 (extract the explicit-inputs leaf
renderer).

**Open questions status:** OQ1 (deadlock) — resolved by construction. OQ2
(back-pressure) — watermark horizon. OQ3 (aspect lanes) — pool instances where
available, else lane priority + separate page space/predecessor chain per
aspect. OQ4 (VST pools) — the concurrency-degree knob. OQ5
(`stalePredecessor`) — verify-at-publish makes "old page stays until atomically
replaced" the natural form; keep prop-16 fallback for RT.

The hard parts, named: the planner must enumerate child-page dependencies *per
range* through the loop/grain maps (prop 18 gives exact maps; `twTrackMix`
already enumerates overlapping clips per page — refactor, not new math), and
interior-node demand dedup (the 2a dedup map already models the key).

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

> **SUPERSEDED (2026-07-19):** the flake was fixed on the edit path (a1e6011),
> and the park/re-enqueue design below is replaced by "Phase 2 REVISED:
> demand-driven dataflow" above. Kept verbatim for history; sub-step 2a
> (`requestPage` dedup) landed and its dedup map carries over.

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

> **All five resolved by "Phase 2 REVISED: demand-driven dataflow"
> (2026-07-19)** — see its "Open questions status" paragraph. Kept for history.

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
