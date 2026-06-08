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

- **`twCapturingSource`** — a `twRandomSource` adapter over *any* linear
  `twComponent`, i.e. "time-stretch / random-access anything placed before an
  `SCut`", not just decoded files.
  *Deferred:* its only consumer was the grain node; `getRandomSource()` is the
  hook it plugs into. Revisit when a non-file source needs grain/warp treatment.

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

## Conventions

- Promote an item to a `plan/proposed/NN_*.md` proposal once it needs real design
  or touches the engine/model non-trivially.
- When an item ships, record it in `plan/STATE.md` and delete it here.
- Keep entries terse — this is a parking lot, not a spec.
