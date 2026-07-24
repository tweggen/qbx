# Proposal 27 — Import-time analysis sidecars: a per-content, file-backed derived-data store

> **Status: DRAFT (2026-07-24).** Overview / architecture — no code yet.
> Prerequisite reading: `26_RUBBERBAND_STRETCH.md` (the stretch/pitch backend
> this would feed), `25_RENDER_TRANSFORM_PUSHDOWN.md` (compose-to-leaf: the
> consumer that most wants cheap resynth), `10_RENDER_CACHE.md` (the *warm-path*
> output cache this sits beneath), `18_EXACT_POSITION_DOMAINS.md` (typed
> positions / exact maps the analysis must be keyed against), and the
> content-epoch invalidation machinery (proposals 15/18-P5). Related object:
> `SExternFile` / `SPlainWave` (`main/objects/wave/`), whose existing
> `previewData_` is the first, ad-hoc instance of the pattern generalized here.

## Motivation — the trigger is *load* latency, not playback quality

Proposal 26 gives us Rubber Band quality, but its offline `study()`+`process()`
runs **per `twGrainSource` construction** — i.e. re-analysis every project load,
for every stretched/pitched clip, on the critical path to a usable session. The
per-(stretch,pitch) **output cache** (proposal 10) helps *warm* replay but does
nothing for cold start at a novel setting: you still pay full analysis.

The cheap, reusable part of any phase-vocoder / sinusoidal engine is the
**analysis** (STFT magnitudes+phases, or magnitudes + per-bin instantaneous
frequency, or tracked partials + residual). That representation is
**ratio-independent** — computed once, valid for *any* stretch/pitch. Persist it
at **import time**, off the critical path; on load, `mmap` it and the only work
left is cheap re-timing + phase propagation. That is the difference between
"project opens instantly and is scrubbable" and "project opens, then grinds."

The DAG cuts our way here: Ableton/Reaper/Cubase re-derive time-stretch per clip
per session. In a content-addressed DAG the analysis is a **cached node output
keyed by (source-content-hash, algo, fft-size, hop, window, sample-rate)** —
computed once per *content*, shared across every clip that references that
source, invalidated precisely by the existing content-epoch mechanism. The heavy
work stops being per-clip-per-session and becomes per-content, forever.

## Scope decision (taken with the requester) — many small files, not one blob

Do **not** build a "one file holds it all" container. The design principle:

- **File handles are cheap** on every target platform (thousands+ open handles
  is a non-issue). The real budget is **simultaneously cached pages** and the
  **ease of OS read-ahead**. Many small, contiguous, aspect-specific files each
  read sequentially give the page cache and prefetcher the easiest job; a single
  giant multiplexed file fights both.
- Therefore: **on-demand generated, file-backed data**, one file per
  *(content × aspect)*, not one archive per project. Generate lazily-but-eagerly
  (see "import-time, not first-play" below), map on demand, let the OS evict.

This also anticipates the requester's expectation of **more per-content and
per-time import-time metadata** beyond time-stretch analysis — the store is a
*family* of aspect sidecars, not a single format:

| Aspect (example) | Ratio-independent? | Consumer |
|---|---|---|
| STFT / IF-vocoder analysis | yes | grain stretch/pitch (26), pushdown (25) |
| Sinusoidal partials + residual | yes | higher-fidelity/formant path (future) |
| Waveform preview (multi-zoom mips) | yes | UI draw — *already* `previewData_`, ad-hoc |
| Onset / transient markers | per-time | warp, quantize, slicing |
| Pitch / f0 track | per-time | pitch-correct, key detection |
| Loudness / RMS envelope | per-time | normalization, auto-gain |

The waveform preview is the proof the pattern already exists in-tree; this
proposal gives it (and its siblings) a uniform home instead of one bespoke
member per object.

## Shape

**Key.** `hash(source-content) × aspect × params × sample-rate`. Content hash
(not filename/mtime) so identical material shares one sidecar and a project-rate
change or a re-import regenerates cleanly. Params (fft/hop/window/algo) are part
of the key *and* stamped in the file header — changing them mints a new file
rather than silently mis-reading an old one.

**Location.** Sidecars live in a project- (or app-cache-) scoped analysis
directory, addressed by key, e.g. `<cache>/<hash[0:2]>/<hash>.<aspect>.<ver>`.
They are **derived** — safe to delete; absence just triggers regeneration.

**Lifecycle.**
1. **Import-time generation, NOT first-play.** A background job on import writes
   each sidecar before the artifact is ever needed for playback. Lazy-on-first-
   load merely *moves* the stall — it must not. (First-play remains a correctness
   fallback if a sidecar is missing/stale, never the happy path.)
2. **Load = map, don't compute.** `twGrainSource` (and future consumers) locate
   the sidecar by key, `mmap` it, and run only the cheap resynth stage.
3. **Invalidation via content-epoch.** A sidecar is bound to the source content
   hash; edits that change content bump the epoch and orphan the old file (LRU-
   evicted from the cache dir). Stale sidecars must never be re-blessed — same
   invariant the page cache already enforces.

**Format.** Versioned, self-describing header: magic, format-version, algo,
fft-size, hop, window-type, channels, source-sample-rate, source-frame-count,
content-hash. Body is the raw aspect payload (planar, mmap-friendly, no parsing).

## The size tax — a first-class constraint, not an afterthought

A full complex STFT is roughly the size of the source audio (often larger). For
a Reaper-scale multitrack project that doubles disk footprint and mmap
bandwidth. Mitigations, chosen per aspect:

- **Magnitude + instantaneous-frequency** at a coarser hop instead of full
  complex STFT — most of the quality at a fraction of the bytes.
- **Sinusoidal / partials + residual** — very compact, though a heavier front
  end.
- Per-time aspects (onsets, f0, loudness) are tiny — near-free.
- The cache dir is size-capped and LRU-evicted; sidecars regenerate on demand.

## Relationship to the existing caches (layering)

```
per-(stretch,pitch) OUTPUT cache   ← proposal 10, warm-path, ratio-DEPENDENT
        ▲  cheap resynth
ANALYSIS SIDECAR (this proposal)   ← durable, ratio-INDEPENDENT, per-content
        ▲  import-time analysis
source PCM (SExternFile)
```

Novel (stretch,pitch) settings become a **fast resynth from cached analysis**
instead of a full re-analysis; the output cache stays exactly as-is on top for
repeated identical settings.

## Staging (each step independently shippable)

1. **Sidecar substrate** — keyed cache dir, versioned header, mmap load, LRU
   eviction, content-hash keying, content-epoch invalidation. No new DSP yet;
   prove it by **migrating `previewData_`** onto it (behavior-preserving).
2. **Import-time analysis job** — background generation of the STFT/IF aspect on
   import; write sidecar before playback demand.
3. **Consumer wiring** — `twGrainSource` (26) locates+maps the sidecar and runs
   only resynth; missing/stale → today's full path as fallback.
4. **Additional aspects** — onsets, f0, loudness envelope as the per-time
   metadata the requester anticipates; each is a new aspect id, no substrate
   change.

Steps 1–2 alone target the stated pain (load stall). The rest is versatility
headroom toward the "Ableton responsiveness + Reaper multitrack weight, on a DAG
representation" goal.

## Open questions

- **Content hash cost at import** vs. incremental hashing during the WAV read.
- **Resynth core**: reuse Rubber Band's own analysis if it can be persisted, or
  fork a serializable phase-vocoder (Signalsmith/MIT) for a sidecar we fully
  control. RB's `study()` state is in-memory and not exposed — likely a fork.
- **Cache scope**: per-project (portable, duplicated across projects sharing a
  sample) vs. app-global content-addressed (dedup, but lifecycle spans projects).
- **Preview migration**: does folding `previewData_` into the substrate disturb
  the known UI/audio-thread race on `SPlainWave` (splainwave.h header note)?
