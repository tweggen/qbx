# The Phase-Vocoder Stretch Engine and its Analysis Sidecars

> **Status: current design (as shipped).** This is a concept document — it
> describes the time-stretch/pitch engine and the derived-data substrate that
> supports it *as they exist today*, so a reader doesn't have to reconstruct
> the picture from the six milestone entries in `plan/STATE.md`.
>
> Design origin: proposal 27 (`plan/proposed/27_ANALYSIS_SIDECARS.md`,
> milestones M0–M5, executed 2026-07-24…25) and its follow-up proposal 28
> (warp markers, `f0`, formant preservation). Where this doc and the proposal
> text disagree, **this doc wins** — the proposal is the intent, this is the
> outcome. Related: `docs/SIGNAL_CHAIN.md`, `docs/contracts/FREEZE_PROTOCOL.md`,
> `docs/EXACT_ARITHMETIC_DESIGN.md`, and the module contracts under
> `tw303a/sidecar/CONTRACT.md` / `tw303a/sources/CONTRACT.md`.

## 1. What it is and why it exists

Smaragd time-stretches and pitch-shifts file-backed clips with an **in-house
paged-resynthesis phase vocoder**, `twPagedVocoder`. It replaced two earlier
approaches:

- the original overlap-add grain stretch (kept as a dependency-free reference,
  `TW_STRETCH_BACKEND=ola`), and
- Rubber Band (the proposal-26 quality backend), **removed entirely 2026-07-26**
  — which also lifted the GPL obligation it imposed.

The engine was written to solve two problems the old model could not:

1. **The load stall.** The previous `twGrainSource` materialized the *entire*
   warped clip into a resident Float32 buffer in its constructor — per clip,
   per `(stretch, pitch)` variant — on the critical path to a usable session.
   Memory was `O(clip × variants)` and the DSP ran up front. A 5-minute stereo
   clip is ~115 MB per variant.
2. **No streaming, transient-aware, random-access stretch node.** Warp markers,
   live re-warping, and transient preservation all need a variable-ratio
   streaming stretch that can resynthesize an arbitrary output window on demand
   — which a materialize-once model cannot express.

The fix is one architectural move: a deterministic phase vocoder whose output
is **bit-identical under any partition of the output range**, so it can be
driven page-by-page on the worker pool. Memory becomes `O(pages)`; the ctor is
`O(1)` in material length; nothing is materialized at load.

## 2. The stretch/pitch engine — `twPagedVocoder`

Header + normative algorithm comment:
`tw303a/sources/include/tw/sources/twpagedvocoder.h`; implementation
`tw303a/sources/src/twpagedvocoder.cc`. Deterministic, single-threaded,
double-precision math with float analysis storage. It **borrows** the source
channel pointers (they must outlive it), touches no Qt, no engine state, and
never runs on the RT thread.

### Pipeline

1. **Lazy Hann-STFT analysis.** STFT at a fixed `fftSize`/`analysisHop`
   (defaults **2048 / 512**), storing per-bin **magnitude + phase** per channel
   and for the mono fold, computed **on demand** and cached in memory.
   Instantaneous frequency is derived on the fly from the phase difference of
   adjacent frames. Analysis is *incremental by construction* — a per-frame
   windowed FFT over resident PCM is cheaper than reading persisted spectra,
   which is why there is **no `stft.if` sidecar** (see §4).

2. **Time-stretch** by `S = stretch × pitchRatio`: fixed synthesis hop
   `Hs = Ha`, a fractional analysis position per synthesis frame, and standard
   IF-based phase propagation.

3. **Identity phase-locking (Laroche/Dolson) with prominence gating.** Bins
   lock to their governing spectral peak only when that peak stands out from
   the frame's median magnitude by `peakProminence` (default **4×**, ~12 dB).
   Bins under non-prominent, noise-floor peaks propagate **free-running**, so
   stochastic material keeps its phase incoherence. This is the fix for the
   comb/metallic coloration reported on noisy sounds (confirmed by ear,
   STATE.md M4).

4. **Cross-channel coherence.** One rotation field is computed from the mono
   fold and applied to every channel, keeping the stereo image phase-coherent.

5. **Keyframed phase re-anchoring** — the one structural novelty. At synthesis
   frames on a fixed grid (`k ≡ 0 mod keyframeInterval`, default **64**) *and*
   at frames aligned with source **onsets** (from the `onsets` sidecar, §4),
   the synthesis phase state **resets to the analysis phases** (a minimum gap
   is enforced so onset-dense material can't collapse the overlap-add). Three
   consequences follow:
   - **Random access with bounded pre-roll** — any output window resynthesizes
     by rolling forward from the nearest keyframe at-or-before it and discarding
     the pre-roll.
   - **Bit-identical output under partition** — every sample depends only on the
     analysis data and its position relative to the fixed keyframe grid, never
     on how the caller chunked its requests. This is the property gate that
     makes the streaming node safe under any worker, in any order.
   - **Transient re-anchoring** — attacks reset to analysis truth, cutting
     smear. (M5 adds an asymmetric rate-1 *protection zone* around each onset in
     the time map, active only when stretching — attacks map ~1:1, stretch is
     stolen from steady regions.)

6. **Pitch** — a Kaiser-windowed sinc resample (LUT-accelerated) of the
   stretched signal; a final output frame depends only on a bounded
   stretched-domain neighborhood, so the bit-exactness property survives the
   pitch stage. `pitchRatio == 1` is a no-op (pitch-free paths are byte-identical
   to the pre-pitch output).

7. **Exact output length.** The caller dictates the output length as
   `floor(inLen × stretchFrac)` computed rationally (proposal 18 `Fraction`),
   pre-zeroes the destination, and the vocoder writes only what the material
   backs — the tail stays zero. The render-boundary contract is unchanged from
   the old engine.

### Config (`twPagedVocoder::Config`)

| Field | Default | Meaning |
|---|---|---|
| `fftSize` | 2048 | STFT size, power of two ≥ 256 |
| `analysisHop` | 512 | analysis hop ≤ fftSize/2; also the synthesis hop |
| `keyframeInterval` | 64 | synthesis frames per fixed-grid keyframe |
| `peakProminence` | 4.0 | peak-vs-frame-median lock threshold (~12 dB) |
| `userMap` | empty | proposal 28 W1 warp map: `(out, in)` breakpoints in the internal stretched domain; empty = uniform stretch |
| `preserveFormants` | false | proposal 28 W4 opt-in cepstral formant preservation for the pitch stage |

Entry points: `render(outStart, len, dst)` (paged) and the static
`warpOffline(...)` convenience (whole-signal, used by the non-streaming path).

## 3. Streaming integration — `twGrainSource`

`tw303a/sources/src/twgrainsource.cc`. When the vocoder backend is active over
a shareable planar source, the grain source **materializes nothing**:

- The ctor computes exact geometry (`nFrames_` via `twWarpMap::srcToWarpedFloor`,
  bit-identical to `floor(inLen × stretch)` when there are no anchors) and sets
  up a `StreamState` — it does **not** synthesize. This makes the ctor `O(1)` in
  material length.
- `read()` renders **aligned 65536-frame blocks on demand** through the vocoder
  into a small LRU (`StreamState`, `kBlock = 65536`, `kMaxBlocks = 4`). Working
  set is bounded by concurrent pages, independent of clip length.
- The RT audio thread never renders — it reads frozen pages only
  (`docs/contracts/FREEZE_PROTOCOL.md`).
- **Lifetime:** the borrowed source PCM could outlive its owner during a queued
  freeze (the old copy-in-ctor safety net is gone), so `StreamState` co-owns the
  source via `std::shared_ptr<const twRandomSource> srcRef` (`twRandomSource::
  sharedRef()`); the borrowed `chans` planar views are valid for that lifetime.

The resident `data_` buffer path still exists, but only as the **fallback**:
the OLA backend, non-planar/non-shared sources, a `warp.pcm` cache hit (§4), and
the offline whole-signal vocoder path all materialize into `data_`. The
streaming vocoder path is the default for file-backed content.

## 4. The analysis-sidecar substrate (QAF)

Module `tw303a/sidecar/` (`tw_sidecar`, depends on `tw/core` only).
Contract: `tw303a/sidecar/CONTRACT.md`. It is a keyed, versioned store of
**derived data** — precomputed analyses of source PCM that can be regenerated
at will. It never alters engine output, only latency (invariant 4).

- **Container (QAF).** `twqaf.{h,cc}`: a self-describing, CRC-protected file,
  one per `(content × aspect × params)`. 144-byte little-endian header (magic
  `QAF1`, aspect id + version, 128-bit content hash, source geometry, record
  stride/count/hop, params blob, payload region, header CRC-32). Writes are
  atomic (`<path>.tmp` → fsync → rename); any validation failure on open makes
  the file count as **absent** — a bad sidecar is never observed or repaired.
  Access pattern: random start (fixed-stride records → `offset = payload +
  rec·stride`) then sequential read. *(The variable-stride seek-table fields
  exist but are unused — no shipped aspect needs them.)*
- **Store.** `twsidecarstore.{h,cc}`: app-global, content-addressed
  `<hh>/<hash>.<aspect>.<paramshash>.qaf`, size-capped (2 GiB default),
  LRU-by-mtime eviction that **skips undeletable/mapped files and retries**
  (Windows-safe). Total identity match on load (aspect id + version + content
  hash + params hash); an aspect-version mismatch deletes on sight. Root set at
  startup from the per-user cache dir; `SMARAGD_SIDECAR_DIR=<path>|off`
  relocates or disables it (disabled = misses/no-ops, output unchanged).
- **Content keying.** `tw/core/twcontenthash.{h,cc}`: **MurmurHash3 x64-128**
  (in-tree, no external dep; golden-pinned) over the fully decoded planar
  Float32 PCM, folded into `twSampleSource::loadWav()` — one pass, no extra
  I/O, independent of filename/mtime. The same material shares one sidecar
  across projects.

### Shipped aspects (`tw303a/sidecar/include/tw/sidecar/twaspects.h`)

| Aspect | Ver | Payload | Origin |
|---|---|---|---|
| `preview.peaks` | 1 | 2-byte {int8 min, int8 max} per probe — the former ad-hoc `previewData_` | 27 M0 |
| `onsets` | 4 | packed 12-byte {u64 attack-centered pos, f32 salience} | 27 M1 / 28 |
| `loudness` | 1 | f32 RMS envelope, 10 ms hop | 27 M1 |
| `f0` | 2 | f32 Hz per hop (YIN; 0 = unvoiced) | 28 W3 |
| `warp.pcm` | 5 | planar Float32 finished warp output | 27 M2 |

**`stft.if` was deliberately dropped.** It was the proposal's centerpiece — a
persisted spectral aspect the vocoder would read. In practice the vocoder's
incremental in-memory analysis (a ~50 µs windowed FFT over resident PCM) beat
reading persisted spectra, so no such aspect exists and the vocoder needs no
spectral readiness gating.

**`warp.pcm` — the durable load-stall cache (M2).** A byte copy of a finished
warp, keyed by content × backend × exact stretch × pitchCents × onset/anchor
fingerprints. On a hit the grain-source ctor adopts the finished PCM and skips
synthesis; on a miss it synthesizes (materialize path) and persists. It is
strictly a byte cache — a stored warp and a recomputed warp are byte-identical
(the M2 `cmp` gate). Superseded as the primary path by M5 streaming (the
streaming path returns before the cache), it remains a valid zero-DSP top layer
for the materialize backends.

## 5. Readiness protocol

Per-time aspects (`onsets`, `loudness`, `f0`) are generated at import on a
**background analysis lane** in `CaptureRevalidator` (arbitrated below
revalidation, above graph work; drained by `pauseBackground()` so offline
renders stay exact). While a clip's required per-time data is not yet ready:

- its pages freeze as an explicit **silent page** (via `setRenderReady` /
  `SCut::setRenderGateReady`) — consumers never block, and
- the clip shows an amber **"analyzing…"** badge.

On completion the affected page ranges are invalidated and the scheduler
re-freezes on demand; the badge clears. The protocol is **latch-free and
order-independent** — whichever of demand/completion happens first, the system
converges. Note this readiness dance gates the *per-time aspects*, **not** the
stretch itself: the vocoder analyzes synchronously in memory, so there is no
"spectral not-ready → silence → re-freeze" cycle for warping.

## 6. Determinism and backend selection

- `TW_STRETCH_BACKEND` is read **once per process** (`twgrainsource.cc
  stretchBackend()`): `ola` → legacy overlap-add; anything else → vocoder
  (the M5 default). Warp anchors force the vocoder regardless of the env var.
  The backend tag is part of the `warp.pcm` cache key, so cached warps can
  never cross backends. Value `1` is reserved for the retired Rubber Band path
  and never reused.
- **One code path per run**, double-precision **scalar** (in-tree radix-2
  twiddle-table FFT — no pffft, no AVX2/NEON; the RelWithDebInfo build plus
  algorithmic wins made SIMD unnecessary). Byte-exact gates are per-machine, as
  before; cross-machine bit-equality is not promised.
- Determinism is gated by byte-level `cmp` of rendered WAVs and by
  `repeat_test.sh` over grain cases across worker counts {1,4,8,16}.

## 7. Extensions built on this substrate (proposal 28)

The proposal-27 "M6 headroom" items each became a proposal-28 milestone on the
same substrate: **user warp maps / markers** (`Config::userMap`, warped-domain
frame destinations, snapped to onsets), the **`f0`** aspect, and opt-in
**formant preservation** (`Config::preserveFormants`, W4). See
`plan/proposed/28_*.md` and the `plan/STATE.md` W0–W5 entries for detail.

## 8. Source map

| Concept | Where |
|---|---|
| Vocoder algorithm (normative comment) | `tw303a/sources/include/tw/sources/twpagedvocoder.h` |
| Vocoder implementation | `tw303a/sources/src/twpagedvocoder.cc` |
| Streaming grain integration, `warp.pcm` cache, backend select | `tw303a/sources/src/twgrainsource.cc` |
| QAF container | `tw303a/sidecar/{include/tw/sidecar/twqaf.h,src/twqaf.cc}` |
| Sidecar store (LRU, eviction, keying) | `tw303a/sidecar/src/twsidecarstore.cc` |
| Aspect registry (ids, versions, layouts) | `tw303a/sidecar/include/tw/sidecar/twaspects.h` |
| Analyzers (onsets/loudness/f0 DSP) | `tw303a/sidecar/{include/tw/sidecar/twanalyzers.h,src/twanalyzers.cc}` |
| Content hashing | `tw303a/core/{include/tw/core/twcontenthash.h,src/twcontenthash.cc}` |
| Readiness gate + "analyzing…" badge | `main/objects/wave/src/splainwave.cpp`, `…/splainwaverndrinline.cpp` |
| Store root / env var | `main/shell/src/sapplication.cpp` |
| Substrate contract & invariants | `tw303a/sidecar/CONTRACT.md` |
| Milestone history | `plan/STATE.md` (§§ "Proposal 27 M0…M5", "Proposal 28 W0…") |
