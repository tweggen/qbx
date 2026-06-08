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

## Conventions

- Promote an item to a `plan/proposed/NN_*.md` proposal once it needs real design
  or touches the engine/model non-trivially.
- When an item ships, record it in `plan/STATE.md` and delete it here.
- Keep entries terse — this is a parking lot, not a spec.
