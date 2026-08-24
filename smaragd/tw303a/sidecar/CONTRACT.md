# tw/sidecar — CONTRACT

Purpose: the derived-data substrate (proposal 27). A self-describing,
versioned, CRC-protected container (QAF) plus a size-capped, LRU-evicted
directory store keyed by (content hash × aspect id × params hash). Everything
it holds is DERIVED — a precomputed analysis of source PCM (e.g. waveform
preview peaks) that can be regenerated at will.

Public headers: twqaf.h (container: `twCrc32`, `twQafWriter`, `twQafReader`),
twsidecarstore.h (`twSidecarStore`), twaspects.h (the closed aspect-id/version
registry — constants only).

Depends on: tw/core (twcontenthash, twlog). Forbidden: everything else. No Qt,
no engine graph state — std::filesystem and cstdio only.

Threading: a `twQafReader`/`twQafWriter` instance is single-threaded; distinct
instances on distinct files are freely concurrent. `twSidecarStore` serializes
every method under one mutex; callers may use it from any non-RT thread.

Invariants:
1. Sidecars are derived. Deletion is always safe; absence of a file simply
   means the consumer recomputes and (optionally) re-stores. The store is a
   cache, never a source of truth.
2. A failed or partial file is never observable and is never "repaired".
   Writers produce files atomically (write to a PRIVATE temp
   "<path>.<pid>.<seq>.tmp", fsync, rename over the target); any validation
   failure on open (magic, format version, header CRC, ascending region bounds
   vs. file size) makes the file count as ABSENT.
   The temp name is unique per WRITER, not per key, and that is load-bearing:
   one store root is shared by every process on the machine, so several
   processes routinely stage the same key at once (`ctest -j` runs the whole
   suite against one store, and every case using the same fixture derives the
   same content hash → the same key). A shared "<path>.tmp" would let two
   writers truncate and interleave into one file, and only the header carries a
   CRC — a torn payload of the right length passes the region-bounds check and
   would be read back as valid analysis data, breaking invariant 4. Eviction
   only ever considers `*.qaf`, so temps are never evicted; a temp leaked by a
   crash between open and rename is collected by deleting the store (inv. 1).
3. Identity match is total: aspect id + aspect version + content hash + params
   hash must all agree for a load hit. A file whose aspectVersion no longer
   matches is orphaned — deleted on sight (it can never become valid again).
4. The store disabled (unset root) is behaviorally identical to the store
   enabled, minus speed: load() misses, store() no-ops, the engine result is
   unchanged. Sidecars must never alter output, only latency.
5. Never called from the RT audio thread — it does file I/O. Callers are
   background/aspect and UI threads only.

How to test: round-trip a `twQafInfo` + payload through `twQafWriter::write`
and `twQafReader::open`/`readRecords`/`readAllPayload`; assert header fields
survive and that byte corruption of the header fails open(). Store tests use a
temp-dir root: verify keying/pathFor spelling, load-miss on version bump (with
on-sight deletion), mtime touch on hit, and that `evictIfNeeded` drops
oldest-mtime `*.qaf` files past the cap while skipping locked ones.

Known debt:
- The aspect registry (twaspects.h) now carries the shipped aspects:
  preview.peaks v2 (proposal 36 B8: the probe envelope folds EVERY channel
  rather than channel 0, and the header's `channels` carries the real source
  width instead of a hard-coded 1; v1 files orphan on sight, because their bytes
  are only accidentally right — they agree with the all-channel fold exactly
  when channel 0 dominates, which every fixture in this repo happens to
  satisfy, and would therefore have been adopted silently), onsets v2
  (NORMALIZED spectral flux — v1's absolute flux
  fired spurious onsets on steady loud material; v1 files orphan on sight),
  loudness v1, and warp.pcm v2 (params blob gained an onsetsHash so warps
  built before/after the onsets sidecar occupy different keys). The `stft.if`
  spectral aspect proposed for the vocoder was DROPPED by design: the paged
  vocoder's analysis is incremental (a lazy windowed FFT over resident PCM
  beats reading persisted spectra), so it needs no analysis sidecar.
- The variable-stride seek-table path is unimplemented: writers always emit
  seekTableOffset = 0 and no aspect uses a variable stride yet.

## Read-side metric derivation (proposal 40 M3b)

`tw/sidecar/twgroovemetrics.h` derives the Feel Flow metric lab's per-hop
series from DECODED "groove.res"/"groove.ev" payloads — pure, deterministic,
no I/O, no store access, and NEVER part of any wire format or store key: a
read-side parameter change re-derives from the same bytes, it does not
re-key or re-run the analysis. Series values are in [0,1] except the
no-data sentinel (< 0), which every consumer must render NEUTRAL, never as
low compliance — a window with fewer than `minWindowEvents` in-category
events is an absence of evidence, not a failure (the design's fill/break
rule, trap 9, made structural). Events past `fusionCeilingMs` are excluded
from sigma/mu statistics and counted by the `outliers` series instead
(section 2.3: past the fusion ceiling "compliance" is the wrong category).
The series ID SET AND ORDER are contractual (the panel rows, the
`assert-feel-flow-panel` grammar and `set-feel-flow-metric` all address
them): compliance, power:<unit>..., rollnorm, sigma, mudrift, outliers,
evconf, score, density — then, when "groove.dyn" records are supplied and
match the res grid (proposal 40 M3c), the Tier B rows: support, tension,
lean, slip, move:<unit>. Tier B rows are ABSENT for a pre-M3c store, never
sentinel-filled ghosts. Gated by `groove_test` section p (closed-form
synthetic records, including the density-decorrelation gate that states the
whole M3b motivation: equal jitter at 2:1 density moves `density` by >= 0.3
and `sigma` by <= 0.05) and section q (the Tier B closed forms and the
pipeline consistency gate).

The "groove.dyn" ENCODER (twgrooveaspect.cc, M3c) has two rules of its own:
phi = arg z is the SAME per-hop quantity the section 3.5 counterTension
summary reads — one physics, gated by the consistency check mean(hypot(
support,tension))/k == counterTension.meanF (measured 0.014 % apart) — and
resampling onto the aspect grid is BIN-AVERAGED, never point-sampled: the
drive is impulsive and a click train's transients land on exactly the
pendulum hops a lerp at the coarser grid would sample (measured: +52 % on
the reference's mean drive before the bin mean).

**"groove.dyn" is at VERSION 2 since proposal 40 M3e** — six float32 per
unit per hop, `{support, tension, cosPhi, sinPhi, slip, dissip}`,
recordStride `nUnits*6*4`. The two new channels are the unit's own PHASE,
cos and sin of the SAME `phi = arg z`, computed per PENDULUM hop and then
bin-averaged by the identical lambda the other four go through.

**A WRAPPED PHASE MAY NEVER BE BIN-AVERAGED, and that is WHY the phase is
two channels rather than one.** phi lives on a circle: a bin straddling the
±pi wrap holds values like {+3.14, −3.14} whose arithmetic mean is 0 — the
diametrically OPPOSITE phase. Averaging cos and sin SEPARATELY is exactly
the circular-mean NUMERATOR, so `hypot(cosPhi, sinPhi) <= 1` always holds
and the shortfall MEASURES the in-bin phase spread (1 = every hop in the bin
agreed; ~0 = they cancelled). A consumer wanting the mean angle takes
`atan2(sinPhi, cosPhi)`; the M3e puppet uses the raw `cosPhi`
UN-renormalized, so an incoherent bin damps the motion by construction
rather than inventing an excursion.

**Why v2 exists at all**: M3c argued sin phi was recoverable read-side from
`hypot(support, tension)`. That is true for the DRIVE-WEIGHTED metrics and
FALSE for an animation — between impulses the drive is ~0, so support and
tension are both ~0 and the phase is unrecoverable at any precision, while
the pendulum is still swinging. Continuous phase therefore has to be
persisted. v1 payloads orphan on sight (the aspect version is part of the
store key) and both analysis jobs' skip-checks re-analyze once on the
version miss — the same mechanism the pre-M3c res+ev store took.

Gated by `groove_test` section q with FOUR assertions, and it takes all four
— **the two obvious ones do not bite, which the watched-failing pass is the
only reason anyone knows**:

1. per record `hypot(cosPhi,sinPhi) <= 1 + 1e-4` (measured range
   **[0.597804, 1.000000]**). A CEILING — zeros satisfy it.
2. the mean `hypot >= 0.5` (measured **0.997696**). This is the one that
   bites an ABSENT channel. It is physical, not tuned: one pendulum hop
   advances `ω·dt ≈ 0.13 rad` and an aspect bin spans a couple of pendulum
   hops, so a bin is coherent by construction and the dips to 0.5978 are
   the few bins that straddle a wrap.
3. the ALIGNMENT: `Σ(support·cosPhi + tension·sinPhi)` exceeds
   `2 × |Σ(support·sinPhi + tension·cosPhi)|` (measured **3.75259 vs
   0.601274**). This is the one that bites a cos/sin SWAP, which 1, 2 and 4
   all miss — magnitude is symmetric under it. Scale-free: `support ≈
   k·F·cos φ` and `tension ≈ k·F·sin φ` come from the same φ, so the right
   pairing sums `Σ k·F` and the wrong one sums `Σ k·F·sin 2φ ≈ 0`; only
   which pairing wins is asserted.
4. the PHASE consistency check — the UNWEIGHTED whole-run mean of the
   exported `sinPhi` reproduces `counterTension.meanSinDeltaPhi` (an
   unweighted per-hop mean of `sin(phaseWrapped)`, which equal-size bin
   means preserve up to the ragged tail bin) within 0.02 ABSOLUTE; measured
   delta **1.05e-09**. Absolute is correct — the value itself is 0.00423908
   and a relative tolerance would be meaningless — but note that the
   tolerance is consequently LARGER THAN THE QUANTITY, so on this fixture
   it passes a channel of zeros (measured delta 0.00423908) and a swapped
   pair (0.00169453). It states "one physics"; 2 and 3 are what enforce it.

`assert-groove-aspect aspect="groove.dyn"` re-derives nUnits from the v2
stride (`recordStride / 24`) and carries the same per-record hypot ceiling.
