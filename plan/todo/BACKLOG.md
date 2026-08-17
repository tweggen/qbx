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
  and ~~**multi-channel** capture (mono for now)~~ — **DONE at proposal 36 B7**:
  `SCut::buildCapture_` captures every channel, with the width taken from the
  content (a mono container still yields a mono capture). The cache is **dormant
  until asset placement** (slice 2) gives it a consumer.
  **The content-addressed cache is now the bigger item, not the smaller one**
  (proposal 36 §7 trap 25, sized at B9): a capture is per PLACEMENT and is
  rebuilt on every invalidation without memoization — measured at 7-9 rebuilds
  per corpus run — and B7 made each rebuilt buffer `channels *` larger, so ten
  placements of one 8-channel asset are ten full-width buffers. Measured on the
  corpus: 230 400 B of resident capture at width 1 against 1 843 200 B at
  width 8, exactly x8.

- **Windowed / streaming sample source** — a fallback for files too large to keep
  fully resident in RAM (`twSampleSource` decodes the whole WAV to planar Float32
  up front).
  *Deferred:* residency is fine for typical material; huge files are the only gap.
  Unblocks very long imports / low-memory targets.

---

## From proposal 05 — track grouping (feature (a), shipped; still in `proposed/`)

Feature (a) is complete, and both leftovers below are now CLOSED. (Feature (b),
live region assets, is the live remaining design in proposal 05 — not a backlog
item.)

- ~~**Group/Ungroup operate on top-level tracks only**~~ — **DONE 2026-08-09.**
  Both gestures are path-addressed and work at any depth. The generalisation was
  not just addressing: `add-track` only appends at the MIXER's top level and
  `reparent-track` refuses a same-container move, so for a nested lane the folder
  has to be created top-level, reparented INTO the target's parent at the
  target's slot, and the target then moved into it. Ungroup promoted children to
  the mixer unconditionally, which would have flung a nested folder's children
  out to the top level; it now promotes into the folder's own parent. Gate:
  `qxa.group_nested_track`, driven through the REAL slots by the new
  `group-track` testkit verb (re-spelling the macro in a script would test the
  script, not the arithmetic).

- ~~**Nested-track solo resolved at the top mixer only**~~ — **DONE 2026-08-09.**
  The audibility rule lives in `app/model/ssolorules.h` and is applied at every
  summing container; `anyTrackSoloed()` walks the whole tree. See the STATE.md
  entry "Solo works inside a group". Gate: `qxa.solo_nested_track`.

---

## From component chain review (2026-06-30)

- ~~**Verbose qWarning() calls in SStdMixer::setNBusses**~~ — **MOOT.**
  `setNBusses` was deleted by proposal 36 B4 along with the per-bus
  instantiation; a mixer's width is `SProject::channels()`. The line numbers
  cited here have not referred to anything since.

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
