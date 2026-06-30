# Backlog

Loose, deferred follow-up items that are real but **do not warrant their own
proposal** — and items orphaned when a proposal moved to `plan/done/` (its
deferrals shouldn't vanish with it). Anything large enough to need design lives
in `plan/proposed/`; the chronological record of what shipped is `plan/STATE.md`;
broad standing gaps (untested ALSA, placeholder backends, no CI, …) live in
CLAUDE.md's *Known Issues* and are not duplicated here.

Each item: one line of *what*, one of *why deferred* / what unblocks it.

---

## From proposal 07 — sample source/reader split (now in `plan/done/`)

- **`twCapturingSource` — class + asset wiring DONE (2026-06-08).** The
  `twRandomSource` that materialises any linear `twComponent` (e.g. a group's
  `twTrackMix`) once into a resident planar buffer, and the `SCut` wiring that
  caches a container content through it with transparent invalidate-on-edit, are
  both in (see STATE.md). *Remaining (optimisations, not blockers):* a
  **content-addressed shared cache** so identical cuts render once instead of each
  owning its own capture (proposal 06 §7 tier 3); a **finer invalidation gate**
  (re-capture only when the captured subtree actually changed, vs every action);
  and **multi-channel** capture (mono for now). The cache is **dormant until asset
  placement** (slice 2) gives it a consumer.

- **Windowed / streaming sample source** — a fallback for files too large to keep
  fully resident in RAM (`twSampleSource` decodes the whole WAV to planar Float32
  up front).
  *Deferred:* residency is fine for typical material; huge files are the only gap.
  Unblocks very long imports / low-memory targets.

---

## From proposal 05 — track grouping (feature (a), shipped; still in `proposed/`)

Feature (a) is complete; these are the two small leftovers. (Feature (b), live
region assets, is the live remaining design in proposal 05 — not a backlog item.)

- **Group/Ungroup operate on top-level tracks only** — you cannot group or ungroup
  a track that is already nested inside a folder.
  *Deferred:* the gesture/menu plumbing resolves the clicked top-level track;
  generalising to arbitrary depth is UI work, no model change.

- **Nested-track solo resolved at the top mixer only** — solo's "silence the
  others" is computed over the mixer's direct children, so soloing inside a folder
  doesn't honour the group hierarchy (proposal 05 §1.3).
  *Deferred:* needs `anyTrackSoloed()` + audibility to walk the whole tree; the
  intrinsic-processing refactor (§0) already landed, so this is the remaining bit.

---

## From component chain review (2026-06-30)

- **Verbose qWarning() calls in SStdMixer::setNBusses** — lines 227, 248, 255, 264 spam stderr during normal operation (e.g. track add/remove). Should be debug-only or removed.
  *Deferred:* cosmetic; does not affect correctness or performance.

- **Speaker input wiring silent before first track added** — twRewire::linkOutput(0) returns NULL until wired, so rewireSpeaker() on empty project gives speaker NULL inputs. Playing outputs nothing (correct but unintuitive). Document in comment or add debug-friendly silence generation.
  *Deferred:* correct current behavior; UX refinement only.

## From offset flow investigation (2026-06-30) — FIXED

- ✅ **FIXED: Grained cut offset domain mismatch** — `SCut::rebuildReader()` did not scale `adjustedStartOffset` by `snap.grainParams.stretch` when grain was active. The loop length was correctly scaled, but the offset was not, causing playback to start at wrong position in the materialized stretched buffer. Fixed by adding offset scaling at line 146 to match seekTo() behavior. See `plan/todo/OFFSET_FLOW_TRACE.md` for detailed analysis.

---

## Conventions

- Promote an item to a `plan/proposed/NN_*.md` proposal once it needs real design
  or touches the engine/model non-trivially.
- When an item ships, record it in `plan/STATE.md` and delete it here.
- Keep entries terse — this is a parking lot, not a spec.
