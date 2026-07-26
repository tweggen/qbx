# Proposal 29: Consolidated follow-ups after M6 (proposals 27 + 28 complete)

> **Status: OPEN (2026-07-26).** Not a design — a routing document. Proposals
> 27 (analysis sidecars, M0–M5) and 28 (warp markers, W0–W5) are fully
> executed; this file collects every thread they left open, plus the standing
> open proposals, so a future session can pick the next milestone without
> re-reading the whole history. Read `plan/STATE.md` (2026-07-24..25 entries)
> for the execution record behind each item.

## 1. Awaiting the requester (no code until they speak)

- **W4 listening verdict — formant preservation on vocal material.** The
  formant-centroid metric says formants hold (706/670 Hz vs source 658 under
  ±1200c); whether it *sounds* right is the requester's call. Toggle
  "Preserve formants" in the clip context menu on a transposed vocal and
  A/B. Failure modes to listen for: hollow/quiet (de-energising), colored
  noise floor. **PGHI ("Phase Vocoder Done Right") is the designated
  escalation** if phase-locking quality disappoints — evaluate only then.
- **Two-marker drag semantics re-test.** The drag-locality fixes (base-
  stretch tail extension + auto start pin, commit `adfe5b4`) implement the
  requester's stated expectations (a/b/c: markers pin, mouse-true motion,
  waveform follows). Verified synthetically; the requester confirmed single
  click+drag works but has not re-run their two-marker drumloop scenario.
- **Start-pin semantics.** Dragging the pin (the auto-added marker at the
  clip's left edge) deliberately slips the window — the one gesture that
  means "move the content origin". If that surprises in practice, options:
  lock the pin behind a modifier, or a distinct handle color.

## 2. Marker UX polish (small, wait for hands-on feedback to prioritize)

- **Onset-snap for marker drags** (deliberately deferred from W2): drags and
  double-click adds currently snap to the GRID; pulling to the amber onset
  ticks (within a pixel threshold, tick wins over grid) is the natural next
  refinement. All the data is at hand (`SPlainWave::onsetsForUi`).
- **Snap bypass modifier** (e.g. Shift-drag = free placement). At coarse
  zoom the grid snap can visibly displace a new marker from the click point.
- **Pin handle vs loop `[]` handle collision:** on looping clips the pin's
  tiled handle coincides with the loop-boundary handles at tile starts.
  Marker hit-test currently wins (runs first). If grabbing one when meaning
  the other annoys, separate the hit zones (e.g. marker strip = top 10 px,
  loop handle only below it).
- **Undo granularity of the first marker add:** the auto pin + the clicked
  marker are TWO undo steps (two actions). A composite/macro action would
  make it one. Cosmetic; SCompositeAction exists and would do.
- **Marker drag live-audio feedback:** drags mutate anchors live (preview
  follows via invalidation); at 70 ms/page post-W5 this should feel fine —
  if not, that reopens the W5 bench.

## 3. f0 consumers (the aspect ships ahead of them by design)

Aspect `"f0"` v2 (YIN, per-hop, 0 = unvoiced) is generated for every
imported wave but has NO consumers yet. Candidates, smallest first:
- **Pitch-curve overlay** in the clip renderer (paint-time read, the
  onsets-tick pattern — lock-free cached read via a UiF0 slot).
- **Key/scale detection** (aggregate f0 histogram → key estimate shown in
  the clip header or extern-file list).
- **Pitch-correct / retune** (quantize f0 trajectory to scale; the vocoder
  already has the per-frame envelope machinery from W4 to do this without
  formant smear). This is a real milestone, not a follow-up — propose
  separately when wanted.

## 4. Standing open proposals (the next big milestones)

- **Proposal 25 — render transform pushdown (DRAFT, was frozen until W1;
  NOW UNBLOCKED).** Scalar ∘ piecewise-linear composes cleanly since
  twWarpMap landed. Re-read the draft against the W1..W5 reality before
  executing (it predates warp maps, WarpPcm v5, and the W4 flag).
- **Proposal 20 — dataflow follow-ups (OPEN).** Preview lanes, pipelining,
  legacy-pull deletion, housekeeping. Unchanged by M6; still the entry
  point for the next ENGINE work.
- **Proposal 21 — realtime dataflow integration (DRAFT v2).** Live inputs /
  live plugin instruments: live lane + capture bridge + frontier contract.
  Architectural outline only.
- **Proposal 17 phase 5 — loop recording** into take stacks (the one
  take-lane phase never executed).

## 5. Performance reserve (re-run `warp_bench` before ANY of this)

W5 (2026-07-25) took the measured wins: pitch stage 3.9×, YIN 2.4×, one
page 199→70 ms. `smaragd/build/bin/warp_bench.exe` is the instrument (in
tree, NOT a gate — 30 s stereo scenarios, prints ms + ×realtime).
Deliberately not done, with their triggers:
- **Explicit SIMD intrinsics / pffft / FFT butterfly vectorization** —
  only if a real workload measurably hurts (stretch-only is 46× realtime).
- Determinism rules if any of it happens: data-parallel vectorization is
  bit-safe; REDUCTIONS must use the fixed-lane pattern (four lanes,
  (l0+l2)+(l1+l3)) and bump the owning aspect/warp version; keep
  `-ffp-contract=off` so x86-64 and aarch64 agree.
- **Capture-content per-time aspects** (proposal 27's M6 note): only when a
  concrete consumer appears.

## 6. Housekeeping (do opportunistically, none urgent)

- **Rubber Band licensing decision — EXECUTED 2026-07-26:** removed
  entirely on the requester's decision (code, CMake discovery, vcpkg
  overlay, docs). GPL obligation lifted; `ola` remains the dependency-free
  reference; warp.pcm backend byte 1 reserved for the retired path.
- **`save-project` does not create parent directories** — qxa cases with
  relative save paths depend on the harness convention `--test-output-dir
  build/test-output/<case>` (bit us 2026-07-25: `mute_survives_reload`
  fails under any other output dir). Either mkpath in
  SSaveProjectAction::apply or pin the convention in the runner.
- **A suite runner script** — the 56-case qxa suite is currently run by
  ad-hoc shell loops (and the full pass exceeds a 10-minute timeout in one
  chunk). A `run_suite.sh` with per-case output dirs and a summary would
  remove a whole class of harness mistakes.
- **`_env.sh`/build docs:** `./build.sh` MUST run from the repo root; from
  `smaragd/` it silently builds nothing (recorded in STATE.md 2026-07-24;
  worth a guard in the script itself).
- **Platform debt** (unchanged, see CLAUDE.md "Known Issues"): ALSA
  untested since refactor, PipeWire/JACK/Pulse placeholders, CoreAudio
  input placeholder, device enumeration phase 7b, no CI.

## 7. Hard-won invariants to keep honoring (the short list)

- No identifier named `emit`/`signals`/`slots`/`foreach` in Qt-linked TUs —
  the W2 invisibility bug (memory: qt-emit-macro-pitfall).
- Any change to synthesis bytes bumps `WarpPcmVersion`; any change to an
  analyzer's output bumps its aspect version. Cached-vs-computed must never
  diverge.
- Renders are byte-deterministic per platform: no fast-math, no FMA
  contraction (`-ffp-contract=off`), fixed-lane reductions only.
- Marker gestures share ONE domain rule: fold into the first loop
  repetition; the displayed extent is `visibRect`, never
  `cut.getDuration()`.
- qxa cases run from `smaragd/tests/cases/`; renders are 16-bit PCM —
  byte-`cmp`, never float-parse.
