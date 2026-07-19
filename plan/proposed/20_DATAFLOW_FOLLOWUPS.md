# Proposal 20: Dataflow follow-ups — preview lanes, pipelining, retirements

> **Status: OPEN (2026-07-19).** Successor to proposal 19, whose demand-driven
> dataflow migration is COMPLETE (stages 1-6, branch `ui-improvements`; see
> `plan/STATE.md` entries "Proposal 19 dataflow stage 1..6" and the design in
> `19_ASYNC_FREEZE_MODEL.md` § "Phase 2 REVISED"). This file collects the
> forward work that proposal 19 deliberately deferred, each item independently
> shippable and gated. Ordering below is the recommended priority.

## Context — what the world looks like now

Consumers (offline render, playback readahead) declare demands
(`CaptureRevalidator::requestGraphPages`); the scheduler expands structural
plans (`twComponent::planPage`, Inv-1 single resolution) into
dependency-counted `PageNode`s executing on the shared worker pool via
`freezePageWithInputs()`; bound input pages are served at two seams
(`twStreamingLatch::copyData` + top of `twComponent::freezePage`); the
predecessor edge gives in-position order + DSP state chaining; the RT thread
is enforced read-only (`twRtThreadGuard`). Deliberately still present:
`cursorMutex_`, the in-node legacy pull (miss fallback), full `pause()` for
the no-scheduler path, and background PREVIEW jobs freezing OUTSIDE the
scheduler.

**Standing gates for every item** (regression, not pass/fail):
`repeat_test.sh takes_group_broadcast 50+` across `SMARAGD_REVAL_WORKERS ∈
{1,4,8,16}`; golden WAVs byte-identical (`cmp`, 16-bit PCM — do NOT parse as
float32); takes_recording_placement ×10; grain_split_delete_crash ×5; module
tests; `python3 tools/check_layering.py`. Hard-won invariants that must
survive any change here are recorded in the stage-4/5 STATE entries and the
memory note `dataflow_freeze_migration` (never demand already-valid pages; no
full-range + per-page demand mix over non-caching components; `planPage`
never under `queueLock_`; renders quiesce background aspects via
`pauseBackground()`).

---

## 1. Preview lanes — aspect-separated freeze state (HIGHEST priority)

**Problem.** Background Preview reval jobs call `freezePreviewPage`, which
restores/captures the SAME `internalState` chain as full-rate freezes
(twcomponent.cc ~694/727) and runs OUTSIDE the scheduler. Consequences:
(a) `cursorMutex_` cannot retire (it is what serializes preview freezes
against graph nodes); (b) renders need `pauseBackground()`; (c) a preview
mid-playback can perturb the playback state chain.

**Design (from 19 § execution classes):** separate page space AND separate
predecessor chain per aspect — either distinct state domains
(`internalState` keyed by aspect) or preview-always-restores-from-snapshot;
for poolable components, a second instance (concurrency-degree N) is the
cleaner form. Convert the Preview reval job to a scheduler demand in a
preview lane so ALL freezing flows through the scheduler.

**Unlocks (do in the same arc, assert-first):**
- `cursorMutex_`/`usesSerialCursor` → try_lock-must-succeed assert for one
  full gate cycle → delete (Inv-3 completion).
- `pauseBackground()` during renders possibly retires (preview lane no
  longer touches playback state) — keep as belt-and-suspenders until the
  goldens prove it redundant.

**Acceptance:** standing gates; goldens bit-identical WITH background
preview jobs left running during a render (the stage-4 nondeterminism
experiment, now expected to pass); no cursorMutex assert fires across the
suite + interactive session.

## 2. Cross-page pipelining via node-result caching

**Problem.** Stage 4 had to drop the full-range look-ahead demand: a
re-demand re-renders NON-CACHING components (`twTrackMix` mints a fresh page
every freeze) out of position order, racing `clip.previousPage`. Renders are
therefore sequential per page (parallel only WITHIN a page), and the
readahead demands conservatively.

**Design.** Make node results the cache for non-caching components: either
give `twTrackMix` a real `outputPages_` cache (respecting range invalidation
— it already computes `twEditRange`s) or have the scheduler keep Done nodes
(with results) in the dedup map until invalidated, so a re-demand is a node
cache hit instead of a re-render. Then restore the full-range render demand
and a readahead look-ahead horizon.

**Acceptance:** standing gates; goldens bit-identical AND deterministic ×3
with the full-range demand restored; render wall-clock measurably ≤ the
sequential form on a multi-track project (record numbers in STATE.md).

## 3. Legacy pull deletion (gated on metrics)

**Precondition:** item 1 done + `graphStats()` shows sustained ZERO misses
across the full suite and a real interactive session. Then: turn a bound-set
miss from "fall through to legacy pull" into "abort node + re-plan"
(structural re-expansion in the worker — the piece stage 3 simplified away),
and delete the recursive pull path (`copyData`'s `freezePage` call, the
`calcOutputTo` live-pull seam in freezes). Keep `requestPage` as the
cache-hit front door. This is the final form of "no synchronous
freeze-calls-freeze recursion".

**Acceptance:** standing gates; a stack-depth guard asserts no
freezePage→freezePage synchronous recursion remains (the proposal-19 Phase 2
acceptance that was deferred); goldens bit-identical.

## 4. Freeze-in-place for capacity-limited components

The classic DAW escape hatch, nearly free in this system: bounce a
capacity-1/0 component's output to pages once and treat it as pure until its
inputs change. Design in 19 § execution classes ("frozen pages standing in
for a component" is the native currency). Needs: a per-component
"frozen-in-place" flag + invalidation hookup + UI affordance (later).

> Related: real-time ingress (live audio inputs, live plugin instruments) is
> drafted separately in `21_REALTIME_DATAFLOW_INTEGRATION.md` (live lane +
> capture bridge + the class-0 frontier contract); its P3/P4 depend on item 5
> below and proposal 08.

## 5. VST / execution-class lanes (design ready, no consumer yet)

The concurrency-degree knob (`∞ | N | 1 | 0`) and capacity-1 lanes with
runs + reset/pre-roll repositioning, real-time-bound components as
capture-only sources — full design in `19_ASYNC_FREEZE_MODEL.md`
§ "Component execution classes". Becomes actionable when plugin hosting
(proposal 08) introduces real single-instance processors. Keep the design in
sync if the scheduler evolves under items 1-3.

## 6. Housekeeping / known debt (small, independent)

- **Headless playback qxa:** none exists — playback changes are gated only
  by unit tests + by-ear checks. Add a `wait-playhead`-style deterministic
  playback test (the old stall-harness verbs referenced in memory
  `headless_playback_test_harness` are gone from `tests/cases/`; recreate).
- **Pre-existing qxa failures (NOT dataflow-related):** the save/load trio —
  `exact_stretch_roundtrip`, `load_project_render`,
  `takes_serialize_roundtrip` fail on `save-project`/`load-project` actions
  (proven on the pre-session baseline). Root-cause separately.
- **Screenshot actions:** environment-dependent (`failed to grab window`);
  either make the runner tolerate it explicitly or gate screenshots behind a
  capability probe, so suite pass-counts stop wobbling.
- **Edit-path try-lock audit leftovers:** `STrack::getTopMostSLinkAt`, the
  child sort comparator, `SObject::getChildrenExtent` (mixed paint/edit) and
  `sstdmixerview.cpp` gesture handlers (waiting for that file to settle) —
  see memory `stale_trylock_duration_class`. Prereq-2's always-fresh
  fallback makes these one-edit-stale at worst; convert opportunistically.
- **Back-pressure formalization:** demands bound outstanding work by
  horizon, but there is no cap on nodes for very long renders (a full-range
  demand after item 2 re-opens this) — cap in-flight nodes / chunk demands.
- **Stress + TSan:** the proposal-19 test plan's randomized edit/playback/
  reval stress with the collision assert compiled in was never built; do it
  when item 1 lands (it is the right moment — preview lanes change the
  concurrency shape).
- **`repeat_test.sh` note:** arg 4 (workers) works again since the knob was
  re-added; `SMARAGD_REVAL_WORKERS=0` = no revalidator = legacy pull paths.
