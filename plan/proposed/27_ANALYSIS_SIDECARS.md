# Proposal 27 — Derived-data sidecars and a paged-resynthesis stretch engine

> **Status: DRAFT v2 (2026-07-24).** Rewritten after design review. v1 proposed
> persisting STFT analysis to accelerate the existing materialize-once grain
> path; review found that half-step keeps the memory wall of the current
> architecture while paying the disk tax of the new one, and leaves a
> two-engine transparency problem (Rubber Band fallback vs. a sidecar-fed
> resynth would *sound different* depending on cache state). v2 commits to the
> full move: a **derived-data sidecar substrate**, **per-time analysis aspects**
> (the industry-proven part — Ableton `.asd`, Reaper `.reapeaks` are exactly
> this), and a **custom paged-resynthesis phase vocoder** that replaces the
> materialize-once model with O(page) streaming resynthesis.
>
> Prerequisite reading: `26_RUBBERBAND_STRETCH.md` (current stretch backend —
> retained as quality reference during this work), `25_RENDER_TRANSFORM_PUSHDOWN.md`
> (compose-to-leaf; its per-variant cost collapses under paged resynthesis),
> `19_ASYNC_FREEZE_MODEL.md` (the demand-driven page scheduler this plugs into),
> `18_EXACT_POSITION_DOMAINS.md` (exact Fraction stretch, the `nFrames_`
> render-boundary contract), `10_RENDER_CACHE.md` (warm-path output cache),
> content-epoch invalidation (15/18-P5). Related object: `SExternFile` /
> `SPlainWave` (`main/objects/wave/`), whose `previewData_` is the first ad-hoc
> instance of the sidecar pattern.

## Decisions taken with the requester (2026-07-24)

1. **Random access is a requirement, not an option.** The end state is
   resynthesis of an arbitrary output window on demand, inside `freezePage`,
   with bounded pre-roll — not whole-clip materialization fed faster.
2. **mmap is not mandated.** Small per-time aspects (onsets, loudness, f0) are
   read fully into memory on open. Large aspects (spectral frames, cached PCM)
   may use mmap or plain chunked reads — an implementation detail per aspect.
   The *format* is what guarantees the access pattern: random start, then
   sequential read.
3. **One shared container format, one file per (content × aspect).** Extensions
   and future plugins add new aspect ids (stereo-width envelope, whatever) as
   new files in the same format — no file database, no rewriting existing
   files, no substrate change. See "The QAF format" below.
4. **Analysis runs in background.** A clip whose required derived data is not
   yet available renders **silence** (via the existing stale/silence fallback)
   with a UI "analyzing…" badge, and is re-frozen automatically when the data
   arrives. Clip-granular, not track-mute (mute is mixer-channel state; we do
   not mutate it from the analysis pipeline).
5. **We write the paged-resynthesis vocoder ourselves.** Performance targets:
   x86-64 (SSE2 baseline, AVX2 dispatch) and ARM64 NEON (Apple M-series).
   Rubber Band remains in-tree as the offline quality reference and A/B
   baseline until the vocoder passes its gates; it is then demoted to a build
   option. Side benefit noted for the record: once RB is no longer linked, the
   GPL obligation it imposed (proposal 26) disappears — relicensing freedom is
   restored if ever wanted.

## Motivation — the stall is an artifact of the architecture, not a missing cache

Proposal 26 gives Rubber Band quality, but `twGrainSource` materializes the
**entire warped clip** into a resident Float32 buffer in its constructor —
per clip, per (stretch,pitch) variant, on the critical path to a usable
session. Two consequences, one per target:

- **Reaper/Cubase multitrack weight:** the wall is resident memory and
  front-loaded DSP, O(clip × variants), where a streaming DAW is O(read-ahead)
  per track. A 5-minute stereo clip at 48 kHz is ~115 MB per transform variant;
  proposal 25 multiplies variants per placement. No amount of cached analysis
  fixes this — synthesis is ~half the phase-vocoder cost and it still runs
  whole-clip up front.
- **Ableton flexibility:** warp markers, live re-warping, transient-aware warp
  modes all need a **variable-ratio streaming stretch node** plus **per-time
  transient metadata**. Neither exists today. Notably, what Ableton actually
  persists in `.asd` is transients/warp/gain — *not* spectra; the stretch
  itself streams. That is the shape we adopt.

The fix is one architectural move with three deliverables:

1. **Substrate** — a keyed, versioned store of derived data files (sidecars),
   invalidated by the existing content-epoch machinery.
2. **Per-time aspects** — onsets/transients, loudness envelope, later f0:
   tiny, engine-independent, the prerequisite for any warp-marker future, and
   a uniform home for `previewData_`.
3. **Paged vocoder** — `twPagedVocoder`: deterministic, phase-locked,
   random-access resynthesis from a spectral-analysis sidecar, running inside
   `freezePage` on the worker pool. Memory becomes O(page); the load stall
   disappears because nothing is materialized at load; proposal 25's variant
   explosion becomes cheap (a variant is a cache key, not a resident buffer).

The two-engine transparency problem from v1 dissolves by construction: the
sidecar is consumed by **the same engine that generates it**. A missing/stale
sidecar means "run the analysis pass first, then resynthesize" — bit-identical
output either way. The cache is transparent to its absence, as a cache must be.

The DAG advantage survives from v1: analysis is keyed by **content**, so every
clip referencing the same material shares one sidecar — computed once per
content. Scope note: this "once, forever" property holds for *imported files*
(`SExternFile`). DAG-generated intermediates (nonlinear-fallback captures under
proposal 25) change content hash on every upstream edit; **spectral sidecars
are therefore generated for file-backed content only** — captures use the
in-memory path. Per-time aspects may later opt in for captures if a use case
appears.

## The QAF format (qbx analysis file)

One file per **(content × aspect × params)**, all sharing one self-describing
container. Directory layout:

```
<cache>/<hash[0:2]>/<contenthash>.<aspect-id>.<paramshash>.qaf
```

Header (fixed-size, CRC-protected):

| field | notes |
|---|---|
| magic `QAF1`, format version | substrate version, not aspect version |
| aspect id | registered short string, e.g. `onsets`, `loudness`, `preview.mips`, `stft.if`, `warp.pcm` |
| aspect payload version | bumping it orphans old files (never mis-read) |
| content hash | XXH3-128 of the decoded source PCM (see keying) |
| source sample rate, channels, frame count | sanity cross-check at open |
| params blob (len + bytes) | canonical aspect-defined serialization; also hashed into the file name |
| record stride, record count, hop (frames/record) | stride > 0 ⇒ O(1) seek: `offset = payload + rec * stride` |
| seek-table offset/len | only for variable-stride aspects; sparse table |
| payload offset/len | payload is contiguous — sequential read after the seek |

Design properties answering the "extensible without a database" question:

- **New metadata = new aspect id = new file.** Existing files are never
  rewritten or appended to. The substrate treats payloads as opaque; an aspect
  *registry* maps id → (reader, generator, stride policy). A future plugin
  registers an aspect and gets keying, background generation, invalidation and
  eviction for free.
- **Random start, then sequential:** fixed-stride records make the seek a
  multiplication; the payload is planar/contiguous so the read after the seek
  is one forward scan. This is the only access pattern the format promises,
  and every consumer we have fits it (a page freeze reads one window; the UI
  reads one zoom level).
- **Small aspects load whole** (a 5-minute clip's onset list is a few KB;
  loudness at 10 ms hop ~120 KB) — plain `read()` into a vector, no mmap.
  `stft.if` and `warp.pcm` are the large aspects and may mmap. Windows note:
  a mapped file cannot be deleted, so LRU eviction skips files with live maps
  and retries — the eviction loop must tolerate that.

**Keying.** Content hash is folded into the existing WAV decode as an
incremental hash over the decoded PCM — one pass, effectively free, and
independent of container/filename/mtime. XXH3-128: non-cryptographic is fine
for a cache key (collision risk negligible, speed matters at import).
Params (fft/hop/window/algo flags) are in the key *and* stamped in the header.

**Lifecycle.** Sidecars are derived data — safe to delete wholesale; absence
triggers regeneration. Invalidation rides the content-epoch mechanism: an edit
that changes file-backed content re-imports (new hash → new key); stale files
age out of the size-capped, LRU-evicted cache dir. Stale sidecars are never
re-blessed — the same invariant the page cache enforces.

**Cache scope:** app-global content-addressed store (dedup across projects),
with the cap and eviction making the lifecycle question moot — a project
opened after eviction just regenerates in background.

## Readiness protocol (background computation)

Generation runs on the shared worker pool as background aspect jobs (the class
that `pauseBackground()` quiesces during renders — an offline render that
*needs* a missing aspect instead demands it synchronously through the normal
job, it does not deadlock on a paused lane).

- **Triggers:** per-time aspects (onsets, loudness, preview) at **import**;
  the spectral aspect (`stft.if`) at **first non-identity edit** of any clip
  over that content — not at import. Rationale: `isIdentity()` clips skip the
  stretch stage entirely and are the common case; unconditional import-time
  STFT spends disk and CPU on material that will never be warped. First-edit
  generation is a one-time background job with a visible affordance, not a
  load stall.
- **Not ready:** the consuming clip's pages freeze as **silence** and the clip
  shows an "analyzing…" badge. No mixer/track state is touched.
- **Ready:** the job completion invalidates the affected clips' page ranges
  (existing range-scoped invalidation, 18-P5); the scheduler re-freezes on
  demand; the badge clears. Order-independent by design — whichever of
  demand/completion happens first, the system converges (no latch to race).

## twPagedVocoder — design sketch

A deterministic phase vocoder with identity phase-locking, built for random
access. Not a port of Rubber Band; a smaller engine whose one structural
novelty is **keyframed phase re-anchoring**:

- **Analysis** (`stft.if` aspect): STFT at fixed fft/hop; store per-bin
  magnitude + instantaneous frequency (mag+IF at a moderate hop beats full
  complex STFT on bytes at equal quality for resynthesis). Ratio-independent:
  one analysis serves every (stretch, pitch).
- **Resynthesis:** standard phase propagation with identity phase-locking
  (lock bin phases to their spectral-peak bin — the known fix for phasiness),
  iFFT + windowed overlap-add.
- **Keyframes:** at a fixed grid of analysis frames (every K frames, K chosen
  so pre-roll ≤ a few thousand samples), phase state is re-anchored to the
  *analysis* phases by rule. Resynthesis of any output window starts at the
  nearest keyframe at-or-before the window and pre-rolls forward, discarding
  until the window start. Consequences:
  - **Random access with bounded pre-roll** — the requirement.
  - **Paged output ≡ whole-clip output, bit-exact,** regardless of access
    order: every sample's value depends only on the analysis data and its
    position relative to the fixed keyframe grid, never on how the caller
    chunked its requests. This is the gate that makes the streaming node safe
    under the page scheduler (any worker, any order).
  - Periodic re-anchoring also bounds phase drift (vocoders do a version of
    this at transients anyway); transient quality is further served by the
    onsets aspect — reset to analysis phase at detected onsets (a keyframe
    forced onto each onset), the classic transient-preservation move.
- **Exactness contract:** output length and the output↔analysis frame mapping
  use the exact `Fraction` stretch (proposal 18); output is defined on
  `[0, nFrames_)` with `nFrames_ = floor(inLen × stretchFrac)` exactly as
  today — the render-boundary contract is unchanged.
- **Pitch:** ratio applied as frequency scaling in resynthesis (plus optional
  resample stage if quality dictates), pitchCents additive through proposal
  25's transform composition. Formant preservation stays a later opt-in.
- **Performance:** FFT via **pffft** (BSD, SIMD, single-file — vendored);
  scalar reference path first, then one SIMD dispatch per platform (AVX2 /
  NEON). Determinism is per-build: one code path chosen at startup, no
  data-dependent dispatch, `repeat_test.sh` N/N on the same machine — the
  same determinism stance as RB's `OptionThreadingNever`. Cross-machine
  bit-equality is not promised (it isn't today either).
- **Threading:** resynthesis state is per-freeze-node, POD-only thread-locals
  if any (MinGW non-trivial-dtor thread_local corrupts the heap — known),
  no Qt on workers.
- **Variable ratio per page** (warp markers) is deliberately designed-for but
  not built until M6: the keyframe grid + per-window ratio parameter is the
  natural seam; proposal 25's `posMap` composition is the consumer.

## Layering after this proposal

```
per-(stretch,pitch) OUTPUT cache      ← proposal 10 pages, in-memory, warm path
        ▲  paged resynthesis (cheap, on demand, O(page))
twPagedVocoder  ⇦ reads ⇨  stft.if SIDECAR   ← durable, ratio-independent, per-content
        ▲  one-time background analysis (first non-identity edit)
source PCM (SExternFile)  +  per-time sidecars (onsets, loudness, preview.mips)
```

`warp.pcm` (M2) sits beside `stft.if` as a durable copy of the *finished*
warped PCM for saved settings — the cheap insurance that kills the load stall
before the vocoder lands, and a still-useful zero-DSP top layer after.

## Milestones

Each independently shippable; suite + `check_layering.py` + `check_logging.py`
green at every gate.

**M0 — Substrate + QAF format.**
New engine module `tw303a/analysis/` (`tw_analysis`, depends on core only; DAG
entry in the layering checker). QAF reader/writer, aspect registry, cache dir,
size-capped LRU eviction (map-aware on Windows), XXH3-128 folded into the
`SExternFile` decode. Prove it by **migrating `previewData_`** onto aspect
`preview.mips`, behavior-preserving (and resolve in passing whether the known
`SPlainWave` UI/audio-thread race note is disturbed — it must not be).
*Gate:* previews pixel/byte-identical; substrate unit tests (round-trip,
version orphaning, eviction under live map); no engine behavior change.

**M1 — Background jobs + readiness + first per-time aspects.**
Background generation on the worker pool with the pauseBackground interplay
specified above; the silence + badge + invalidate-on-ready protocol; aspects
`onsets` (spectral-flux transient detector) and `loudness` (RMS envelope).
*Gate:* qxa case — import, assert sidecar files appear with deterministic
content; a not-ready clip renders silence then converges to correct audio
with no restart (order-independence swept with `repeat_test.sh`).

**M2 — Durable warp-output aspect (`warp.pcm`), the load-stall fix.**
Persist the Rubber Band output buffer (`twGrainSource::data_`) keyed by
content × exact stretch × pitchCents × algo-version; the ctor loads it instead
of re-running study/process when present. Small, engine-agnostic, deletable
later without trace.
*Gate:* renders byte-identical hot vs. cold cache; measured project-load
improvement on a stretched-clips fixture project (record numbers in STATE.md).

**M3 — Vocoder core, offline mode, behind a flag.**
`twPagedVocoder` full-signal mode as an alternate `twGrainSource` backend
(`TW_STRETCH_BACKEND=vocoder|rubberband`), scalar + pffft. Build the **A/B
harness first**: RB and vocoder over the fixture corpus (sawtooth + recorded
voice + a transient-rich loop), scoring RMS bands, frequency accuracy,
transient smear, and a modulation-spectrum "warble" metric (the artifact class
that motivated 26) — so iteration has objective feedback before ears.
*Gate:* all 14 grain qxa cases green under the vocoder backend; A/B report
checked in; deterministic N/N.

**M4 — Keyframed random access + the `stft.if` aspect.**
Analysis-pass writer; `resynth(outStart, outLen)` reading the sidecar with
keyframe pre-roll; SIMD dispatch (AVX2/NEON) with the scalar path kept as the
cross-check oracle.
*Gate:* **paged ≡ whole-clip byte-exact** for randomized page orders and
boundaries (property test); SIMD ≡ scalar within defined tolerance (or exact,
decided at implementation); pre-roll cost measured per page.

**M5 — Engine integration and switchover.**
The grain stage becomes a streaming node: `freezePage` on a non-identity clip
resynthesizes just that page's window from the sidecar (generated per the
readiness protocol). The resident `data_` buffer path and the ctor
materialization are removed for file-backed content; RB demoted to a build
option kept as the reference. Proposal 25's variant cache becomes pages keyed
by `xfHash` — no resident buffer per variant.
*Gate:* full suite; `repeat_test.sh` over grain cases, workers {1,4,8,16};
load stall eliminated on the M2 fixture project *without* `warp.pcm` present;
memory measured on an N-stretched-clips scale fixture (target: independent of
clip length, linear in concurrent pages); byte-exact identity gates untouched
(identity clips never enter this path).

**M6 — Flexibility headroom (each its own small proposal when reached).**
Variable ratio per page → warp markers (with `onsets` as the snap targets);
`f0` aspect; per-clip formant toggle; capture-content opt-in for per-time
aspects.

Ordering notes: M2 is deliberately early and deliberately disposable — it
buys the user-visible win (load time) in days, de-risks the schedule if
M3–M5 take longer than hoped, and exercises the substrate with a large
aspect. Proposal 25 Phases 0–1 can proceed in parallel at any time; its
Phase 2 (stretch pushdown) is best landed after M5.

## Risks

- **Quality bar.** RB R3 is the reference for a reason; a home vocoder that
  measures well can still lose a listening test. Mitigations: the M3 harness
  exists before the engine; RB stays in-tree as the A/B baseline and
  escape-hatch backend until M5's gates pass; the switchover is a flag flip,
  reversible.
- **Effort honesty.** The vocoder core (analysis, phase-locked resynth,
  keyframing) is well-trodden DSP — the risk is not writing it but the
  quality-tuning tail (transients, stereo coherence, extreme ratios). The
  milestone cut is designed so everything before M5 ships value even if
  tuning stalls: substrate, per-time aspects, and the M2 load fix stand alone.
- **Determinism vs. SIMD.** One dispatch per run, scalar oracle in tests;
  the byte-exact gates remain per-machine, as today.
- **Pre-roll cost.** K trades sidecar-adjacent CPU per page fault against
  seek latency; measured at M4, tuned before M5. Pages are 65536 frames;
  a few-thousand-sample pre-roll is small against a page.
- **Stereo phase coherence.** Analyze/resynthesize channels jointly (26's
  bonus observation applies here too); covered by a harness metric.
- **Windows eviction vs. live maps** — handled in the substrate (skip +
  retry); a stuck-forever map would pin cache size, so maps are dropped when
  a reader is destroyed, not cached indefinitely.

## Explicitly dropped from v1

- Import-time STFT for all content (now: first non-identity edit, file-backed
  only).
- "Fork a serializable phase vocoder **or** reuse RB's analysis" as an open
  question — resolved: we write our own, RB's `study()` state stays private.
- mmap as the assumed mechanism everywhere — now per-aspect.
- Sidecar-fed acceleration of the materialize-once ctor as the end state —
  that half-step is exactly what v2 exists to avoid (M2 covers its benefit
  more cheaply; M5 removes its architecture).
