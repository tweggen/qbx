# Proposal 27 — Orchestration plan (execution companion)

> **Status: PLAN (2026-07-24).** This document instructs a Claude Code
> instance (the **orchestrator**) how to execute proposal 27
> (`27_ANALYSIS_SIDECARS.md`, v2) milestone by milestone, iterating until each
> milestone's gates are green, and delegating to **Opus subagents** where a
> smaller/faster model is sufficient. To use: start a session in the repo and
> say *"Read plan/proposed/27_ORCHESTRATION.md and execute the next
> milestone."* The plan is resumable — progress lives in STATE.md and git,
> not in any session's memory.

## 0. Ground rules (bind every milestone, every agent)

1. **The proposal is the spec.** `27_ANALYSIS_SIDECARS.md` v2 decisions are
   settled — do not re-litigate format, keying, keyframing, or backend choice
   mid-implementation. If implementation reveals a genuine contradiction,
   STOP, write the finding into the proposal's Risks section, and surface it
   to the user; do not silently deviate.
2. **Gates are hard.** A milestone is done when every listed gate passes.
   Never weaken a gate to pass it: no widening RMS/peak bands without a
   physically-grounded justification written into the test comment and
   STATE.md; never touch the byte-exact identity gates; never mark a flaky
   test as expected-fail.
3. **Repo laws** (from CLAUDE.md and memory — verify, don't assume):
   - `python tools/check_layering.py` and `python tools/check_logging.py`
     clean before every commit; qxa suite green, run **from
     `smaragd/tests/cases/`** (CWD-relative fixtures — running from
     `smaragd/` silently loads a junk WAV and every RMS assert passes/fails
     bogusly).
   - Concurrency/race gates: `smaragd/tests/repeat_test.sh <bin> <case> N
     [workers]`, swept over workers {1,4,8,16}, N≥50 for anything touching
     the scheduler or readiness protocol.
   - No Qt signals from worker/RT threads; engine stays plain C++ (no
     Q_OBJECT). POD thread_locals only (MinGW non-trivial-dtor thread_local
     corrupts the heap at 3+ threads).
   - The RT callback never renders; freeze-side changes must keep
     `twRtThreadGuard` quiet.
   - Race fixes must be order-independent (remove the latch; don't force an
     ordering).
   - Build via `./build.sh` (Git Bash); vcpkg/Qt wiring is automatic.
4. **One milestone per branch-of-work, one commit (or few logical commits)
   per milestone**, message style matching recent history (imperative,
   subject describes the behavior change). Update `plan/STATE.md` with a
   dated entry at every milestone close: what landed, gate results with
   numbers, deviations. Milestone completion also ticks a checkbox in the
   tracking table below (§6) — edit this file.
5. **Iterate until green.** The inner loop is: implement → build → gates →
   diagnose → fix → repeat. Do not stop on the first failure or hand a
   failing state back to the user. Escalate to the user only when (a) a gate
   has resisted **three** genuinely different diagnoses/fix attempts, (b) the
   fix would require violating rule 1 or 2, or (c) a hardware/ears judgment
   is needed (listening tests, M3/M5 quality sign-off). When escalating,
   leave the tree buildable and write the open state into STATE.md first.

## 1. Agent policy — where Opus is sufficient

The orchestrator (top model) keeps for itself everything **architecture- or
correctness-critical**; it delegates everything **mechanical, exploratory, or
verifiable-by-gate** to Opus agents (`Agent` tool, `model: "opus"`). Batch
independent agents in one message so they run concurrently; run long builds
and test sweeps in background tasks rather than blocking.

**Delegate to Opus (sufficient, faster):**
- Codebase reconnaissance: locating call sites, tracing the `previewData_`
  lifecycle, mapping `SExternFile` decode paths, enumerating grain qxa cases.
  (Explore-type agents; read-only, freely parallel.)
- Test authoring against a written spec: qxa cases, substrate unit tests,
  the M4 randomized page-order property test, harness fixture projects.
- Running gate sweeps and summarizing: build + ctest + qxa + repeat_test.sh
  matrices; report pass/fail + the failing case's raw output, no diagnosis
  required.
- Mechanical implementation with a precise contract: QAF header
  serialization, XXH3 vendoring, LRU eviction bookkeeping, the
  `preview.mips` data-shape migration, CMake/module wiring for
  `tw303a/analysis/`, STATE.md/doc updates.
- Metric implementations in the M3 harness (RMS, spectral centroid,
  transient smear, modulation-spectrum warble) — each has a textbook
  definition; correctness is checkable against synthetic signals.
- pffft vendoring + a benchmark scaffold; NEON/AVX2 *translation* of an
  already-correct, already-tested scalar kernel (the scalar oracle test
  makes this verifiable).

**Keep on the orchestrator (top model):**
- All DSP design and numerics: analysis pass, phase-locking, keyframe
  re-anchoring rule, exact `Fraction` output-frame mapping, pitch path.
- Anything touching the scheduler, freeze path, invalidation, or the
  readiness protocol (M1, M5) — this codebase's entire bug history lives at
  those seams.
- Concurrency diagnosis of any repeat_test.sh flake.
- Gate-failure diagnosis (Opus reports failures; the orchestrator interprets
  them).
- Review of every Opus-produced diff before it merges into the working tree.

**Isolation rule:** parallel Opus agents may share the tree only when
read-only or writing disjoint new files. Any two agents editing existing
files run serialized, or in worktrees merged by the orchestrator. Never let
an agent "fix" a test to make its own code pass — agents implement OR
verify, not both on the same artifact.

## 2. The milestone loop (run this per milestone)

```
1. ORIENT   Read the milestone brief (§3), the relevant proposal sections,
            and STATE.md. Fan out Opus Explore agents for the file/seam
            reconnaissance listed in the brief. Confirm entry criteria.
2. PLAN     Write a short internal work breakdown: which items delegate,
            which stay. Create tasks (TaskCreate) for the milestone's
            checklist so progress survives compaction.
3. BUILD    Implement. Delegate per §1; review every agent diff. Commit
            checkpoints locally as logical units.
4. GATE     Run the full gate checklist from the brief (delegate the sweep,
            keep the verdict). ALL gates, not just the new ones — plus the
            standing suite: ctest, qxa from tests/cases/, layering, logging.
5. FIX      Any red → diagnose (orchestrator), fix, GOTO 4. Track attempt
            count per distinct failure for the §0.5 escalation rule.
6. CLOSE    STATE.md entry with measured numbers; tick §6; commit; report
            the milestone summary to the user. Proceed to the next milestone
            only if the user asked for multi-milestone execution; otherwise
            stop here.
```

## 3. Milestone briefs

Gates listed here are the operational checklist; the proposal's gate prose is
authoritative if they ever diverge.

### M0 — Substrate + QAF format
- **Entry:** none (first milestone).
- **Recon (Opus, parallel):** `previewData_` full lifecycle
  (`SPlainWave`/`SExternFile`, the header's UI/audio-thread race note);
  WAV-decode path for hash folding; module DAG + `check_layering.py`
  registration mechanics for a new `tw_analysis` lib.
- **Orchestrator:** QAF header layout + reader/writer API, aspect registry
  design, eviction concurrency (map-aware skip+retry on Windows), where the
  hash accumulates in the decode.
- **Delegate:** XXH3 vendoring; serialization code; unit tests (round-trip,
  version orphaning, eviction-under-live-map); CMake/DAG wiring; the
  mechanical parts of the `preview.mips` migration.
- **Gates:** previews byte/pixel-identical pre/post migration; substrate
  unit tests green; standing suite green; explicitly confirm the SPlainWave
  race note is not disturbed (document the argument in the commit).

### M1 — Background jobs + readiness + onsets/loudness
- **Entry:** M0 closed.
- **Recon (Opus):** background-aspect-job class and `pauseBackground()`
  semantics in the revalidator; range-scoped invalidation entry points
  (18-P5); where a clip-level UI badge can hang off existing repaint paths.
- **Orchestrator:** the readiness protocol itself (silence → converge on
  ready, order-independent, no latch); job scheduling + render-quiesce
  interplay; invalidation wiring.
- **Delegate:** spectral-flux onset detector + RMS-envelope generator
  against written specs (validated on synthetic fixtures: clicks at known
  positions, amplitude ramps); the qxa cases; the badge painting.
- **Gates:** qxa — import produces sidecars with deterministic content;
  not-ready clip renders silence then converges without restart;
  `repeat_test.sh` on the convergence case, N≥50, workers {1,4,8,16};
  standing suite.

### M2 — Durable `warp.pcm` aspect (load-stall fix)
- **Entry:** M0 closed (M1 not required).
- **Orchestrator:** cache-key composition (content × exact stretch ×
  pitchCents × algo-version) and the ctor short-circuit placement in
  `twGrainSource`.
- **Delegate:** the read/write plumbing on the substrate; a stretched-clips
  fixture project; load-time measurement harness.
- **Gates:** renders **byte-identical** hot vs. cold cache (cmp of rendered
  WAVs); measured load improvement recorded with numbers in STATE.md;
  standing suite; grain cases green both cache states.

### M3 — Vocoder core (offline, flagged) + A/B harness
- **Entry:** M0 closed. **Build the harness before the engine.**
- **Delegate (harness):** metric implementations (RMS bands, frequency
  accuracy, transient smear, modulation-spectrum warble), each unit-tested
  on synthetic signals; corpus assembly (sawtooth fixture + a recorded-voice
  fixture + a transient-rich loop); RB-vs-candidate report generator.
- **Orchestrator (engine):** STFT analysis, mag+IF representation, identity
  phase-locking, resynthesis, exact `nFrames_` clamp, the
  `TW_STRETCH_BACKEND` flag; scalar first, pffft-backed FFT.
- **Delegate:** pffft vendoring/build wiring; running the corpus sweeps.
- **Gates:** all 14 grain qxa cases green under `vocoder` backend; A/B
  report generated and committed (numbers vs. RB on every metric —
  parity not required at M3, but every regression quantified);
  deterministic N/N repeat runs; RB backend still default and untouched
  (byte-exact vs. pre-M3).
- **Escalation note:** "measures close but sounds worse" is a §0.5(c)
  user-ears decision — present the report and samples, don't self-certify.

### M4 — Keyframed random access + `stft.if` sidecar
- **Entry:** M3 closed.
- **Orchestrator:** keyframe grid + re-anchoring rule; `resynth(outStart,
  outLen)` with pre-roll; the bit-exactness argument (output depends only on
  analysis data + keyframe-relative position, never on request chunking);
  exact Fraction output↔analysis mapping.
- **Delegate:** `stft.if` writer on the substrate; the randomized
  page-order/boundary property test (spec: any partition of [0,nFrames)
  into windows, any order, concatenation byte-equals whole-clip); SIMD
  translation of the tested scalar kernels + scalar-vs-SIMD oracle test;
  pre-roll cost benchmark.
- **Gates:** property test green (many seeds — note the no-Date/random
  constraint applies to Workflow scripts, not C++ tests; seed the C++ test
  deterministically); SIMD ≡ scalar per the tolerance decided and documented
  at implementation; pre-roll cost measured and recorded; standing suite.

### M5 — Streaming integration + switchover
- **Entry:** M1, M4 closed. The most dangerous milestone — scheduler seams.
- **Orchestrator:** everything: freezePage-side paged resynthesis, per-node
  state ownership (reader-owns-upstream rule), readiness-protocol
  integration for `stft.if`, removal of ctor materialization for file-backed
  content, RB demotion to build option, interaction with the input-cursor
  serialization and stale-page fallback.
- **Delegate:** N-stretched-clips scale fixture; memory measurement runs;
  full sweep execution; docs updates (CLAUDE.md deps/GPL note, module
  CONTRACT.md files, ARCHITECTURE.md).
- **Gates:** full suite; grain cases via repeat_test.sh N≥50 × workers
  {1,4,8,16}; load stall eliminated on the M2 fixture **with `warp.pcm`
  absent**; memory independent of clip length / linear in concurrent pages
  (numbers in STATE.md); byte-exact identity gates untouched; standing
  suite; a final listening sign-off from the user before RB is demoted
  (§0.5(c)).

### M6 — deliberately out of scope
Warp markers, f0, formants each get their own proposal when reached. The
orchestrator stops after M5 and reports.

## 4. Failure & flake protocol

- A flake at any worker count is a real bug (order-independence rule) —
  never rerun-until-green. Diagnose on the orchestrator; the historical
  cluster to suspect first: shared cursors, non-atomic refcounts, Qt on
  workers, thread_local dtors, latch races.
- If a gate failure implicates pre-existing code (not this proposal's),
  record it in STATE.md as a separate finding, fix it in its own commit if
  small, or surface it to the user if invasive. Do not fold unrelated fixes
  into milestone commits.
- Build breakage on the vcpkg/MinGW seam: check the overlay-port pattern
  (`smaragd/vcpkg-overlays/`) before fighting upstream ports.

## 5. Context & continuity

- Use TaskCreate/TaskUpdate per milestone checklist item; the task list plus
  STATE.md plus §6 below are the durable state — assume any session may be
  compacted or replaced mid-milestone.
- On resume: read §6, STATE.md tail, `git log --oneline -15`, and the task
  list before touching code.

## 6. Progress tracker (edit in place at each CLOSE)

| Milestone | Status | Closed on | Commit | Notes |
|---|---|---|---|---|
| M0 substrate + QAF | ✅ done | 2026-07-24 | (this commit) | Module named `sidecar` not `analysis` (name collision with test-metrics module); in-tree MurmurHash3 x64-128 instead of vendored xxhash; aspect id `preview.peaks` not `preview.mips` (existing preview is single-resolution). Gates: sidecar_test green incl. golden pin; ctest 64/65 (the 1 = pre-existing documented asset_window_shifted_content flake, passed on rerun, out of scope); layering+logging clean. See STATE.md 2026-07-24 M0 entry. |
| M1 jobs + readiness + per-time aspects | ☐ not started | | | |
| M2 warp.pcm load fix | ☐ not started | | | |
| M3 vocoder core + harness | ☐ not started | | | |
| M4 random access + stft.if | ☐ not started | | | |
| M5 streaming switchover | ☐ not started | | | |
