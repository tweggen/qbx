# Proposal 28 — M6: warp markers and analysis-driven flexibility

> **Status: DRAFT (2026-07-25).** The proposal-27 "M6 headroom" items, planned
> as their own milestone track now that M0–M5 are closed (STATE.md
> 2026-07-24/25). Prerequisite reading: `27_ANALYSIS_SIDECARS.md` (executed
> M0–M5), `27_ORCHESTRATION.md` (the execution discipline this track reuses),
> `18_EXACT_POSITION_DOMAINS.md` (typed positions / exact Fractions — the
> rules every new time quantity here must obey), `25_RENDER_TRANSFORM_PUSHDOWN.md`
> (DRAFT — composes with, does not block). The engine substrate this builds
> on: `twPagedVocoder` (tw/sources/twpagedvocoder.h) and the onsets/loudness
> sidecar aspects.

## The one-sentence design insight

M5's transient-preserving time map made the vocoder a renderer of arbitrary
monotone piecewise-linear output→input time maps — deterministic, keyframed
at breakpoints, bit-exact under partition — so **warp markers are not a new
engine: they are user-authored entries in the map the engine already
renders.** M6 is therefore mostly *seam* work (params, domains,
serialization, invalidation) and *UI* work, with one real DSP milestone
(marker-grade onset quality) and two small aspect additions.

## Goal, stated against the original target

"Flexible as Ableton" — the remaining gap after M0–M5 is user-facing
warping: pin a source position to a timeline position, drag it, hear the
result; snap those pins to detected transients. "Reaper-scale performance"
is already banked (streaming O(blocks) rendering); M6 must not regress it.

## Milestones

### W0 — Marker-grade onset detection (v3)

The v2 detector is deterministic and safe for keyframes, but its
precision/recall on musical material is unproven (49 marks on a pathological
crescendo fixture; dense fires possible on beating tonal material). Markers
shown to users must mostly be RIGHT.

- Two-tier output with **salience**: aspect `onsets` v3 — record becomes
  `{ uint64 pos, float32 salience }` (stride 12). Salience = normalized flux
  at the detection, so consumers filter: keyframes take everything (M5
  behavior preserved — dense-but-cheap), the UI shows only
  salience ≥ threshold.
- **Ground-truth harness** (delegate): a labeled synthetic corpus — click
  patterns, drum-like bursts over tonal beds, soft/legato attacks, the
  crescendo trap, steady tones — with known onset positions; precision /
  recall / F-score computation in `analyzers_test`.
- Detector work (orchestrator): whatever v3 needs to hit the gates — the
  candidates from the literature are superflux-style maximum filtering
  across bins and a short adaptive-whitening memory; decide empirically
  against the harness, never by taste.
- **Gates:** F ≥ 0.9 on the strong-attack corpus at the UI salience
  threshold; ZERO UI-tier marks on steady tone and on the crescendo trap;
  all M5 keyframe behavior byte-unchanged for consumers that take the full
  set (the vocoder ignores salience); OnsetsVersion 3 orphaning verified;
  standing suite + sweeps.

### W1 — Engine seam: user warp maps as first-class clip state

- `twGrainParams` gains an optional **anchor list**: pairs of
  (source position, warped position), **exact integers** in their native
  domains per proposal 18 discipline — the map is born from user gestures
  (snapped positions), so it is integral by nature; no doubles are stored.
  Scalar `stretch` remains the degenerate no-anchor case; with anchors, the
  effective end-length derives from the LAST anchor pair exactly (the
  proposal-18 render-boundary rule generalized from `floor(len × frac)` to
  the map's final span).
- `twPagedVocoder` consumes the merged map: user anchors ∪ transient
  protection zones (protection clamps INSIDE user segments — a user anchor
  is authoritative, protection never moves one). Keyframes at every
  breakpoint, exactly as M5 does.
- **Cache/dedup keys:** the warp-params blob (materialize paths) and any
  future variant keys gain a map fingerprint (hash of the exact anchor
  pairs); streaming needs nothing (renders from the live map).
- **Domain math (the invasive part):** proposal 18 made the warped domain a
  scalar multiple of source (`WarpedPos = srcPos · stretch`). With a map,
  warped↔source conversion goes through map evaluation. One conversion
  authority: a `twTimeMap`-family object built from the anchors, shared by
  preview, playback, freeze and invalidation (`mapChildRangesToSelf` must
  fold the piecewise map exactly as it folds stretch today — the range-
  scoped invalidation walk is the seam that must not drift).
- **Serialization:** anchors as exact integer pairs in the `.qxp`;
  load→save idempotent (the proposal-18 test discipline).
- **Gates:** vocoder_test extended — paged ≡ whole under user maps (any
  partition, fresh instances); exact end-length from anchors; qxa
  serialize-roundtrip; byte-exact identity gates untouched (no anchors ⇒
  no change anywhere); standing suite + sweeps {1,4,8,16}.

### W2 — UI: marker editing on clips

- Onset tick marks on wave clips (salience-filtered from the v3 aspect,
  read at paint like `isAnalyzing()` — lock-free).
- Create/drag/delete warp markers; drag snaps to onset ticks and the grid;
  marker edits are `SAction`s (undo/redo) that update the anchor list and
  fire `invalidateRenderPathRange` over exactly the affected span — the
  proposal-18-P5 range machinery is the payoff here: dragging one marker
  invalidates one span, not the clip.
- Live feedback while dragging rides the existing edit→invalidate→refreeze
  loop (measure the latency; this is the W4 trigger if it disappoints).
- **Gates (qxa-driven, per M1's verb pattern):** new verbs
  `add-warp-marker` / `move-warp-marker` / `assert-warp-anchor`; a case
  that pins CONTENT TIMING — place a click at a known source position, warp
  it to a target timeline position, render, assert the click's energy lands
  in the target window (the first test in the suite that asserts *musical
  placement* of warped audio); undo/redo roundtrip; repeat sweeps on the
  marker-edit case.

### W3 — f0 aspect (small, self-contained)

- Aspect `f0` v1: per-hop fundamental estimate (autocorrelation/YIN-lite,
  deterministic, source-rate), float32 records, 0 = unvoiced. Import-time
  generation beside onsets/loudness. Consumers arrive later (key detection,
  pitch-correct); the UI may show a pitch curve overlay if trivial.
- **Gates:** oracle tests (synthetic tones incl. vibrato and octave traps —
  the classic halving/doubling failure), determinism, sidecar plumbing
  (versioning/orphaning) — the M0 substrate makes this a small milestone.

### W4 — Per-clip formant preservation (opt-in)

- The proposal-26 decision stands: default OFF (preservation colours and
  de-energises general material — measured 0.52× RMS back then). W4 adds
  the opt-in for vocal material: spectral-envelope estimation (cepstral
  liftering, deterministic) and envelope-preserving magnitude warping in
  the vocoder's pitch stage; per-clip serialized flag + UI toggle.
- **Gates:** formant metric in warp_ab (vowel-like fixture: envelope peak
  positions unchanged under ±1200c when ON); the proposal-26 trap gated —
  RMS with preservation ON stays ≈1.0× (no de-energising); OFF path
  byte-identical to pre-W4; listening check with the requester (voice
  material is the whole point).

### W5 — Conditional reserve (explicitly gated on measured need)

Not scheduled work — tripwires:
- **SIMD/pffft** if W2's drag-feedback latency measurably disappoints
  (per-page render cost is the budget; scalar -O2 has been sufficient
  through M5).
- **PGHI** ("Phase Vocoder Done Right") evaluated only if the requester's
  listening flags quality the current phase-locking can't reach.
- **Capture-content per-time aspects** (the M6 note from proposal 27) only
  when a concrete consumer appears.

## Open questions (settle with the requester before W1)

1. **Marker destination domain: timeline frames or beats?** Ableton warps
   against the beat grid. Smaragd has bar-derived integer lengths
   (proposal 18) but no first-class tempo-map object in the render path.
   Recommendation: W1 stores warped-domain FRAMES (exact today, convertible
   later); a beat-native destination becomes its own proposal when a tempo
   map exists — do not let M6 grow a tempo system as a side effect.
2. **Interaction with proposal 25 (pushdown):** stretch composition through
   containers currently composes scalar Fractions. Scalar ∘ piecewise-linear
   is still piecewise-linear, so 25-Phase-2 composes cleanly AFTER W1 — but
   the two should not land in the same milestone. Recommendation: 25 stays
   frozen until W1 ships.
3. **Marker semantics at the clip window edge** (a marker dragged past the
   cut window; loop-tiled clips): propose clamping markers to the content
   window and letting `twLoopMap` tile the warped result — decide at W1
   design review.

## Execution notes (reusing the 27 discipline)

Same loop, same rules as `27_ORCHESTRATION.md` (gates are hard, flakes are
real bugs, orchestrator owns DSP/seams, Opus agents own recon/tests/
mechanical work, one milestone per close, listening judgments belong to the
requester). Suggested delegation per milestone is inline above. Effort
shape: W0 and W1 are the substantial ones (W1 touches the domain seams —
budget it like an M5, not an M2); W2 is UI-heavy but rides existing
machinery; W3 small; W4 medium DSP with a hard quality gate.

Tracker: add a W-row table to `27_ORCHESTRATION.md` §6 (or a §6b) at W0
start, same close ritual (STATE.md entry, tick, commit, push).
