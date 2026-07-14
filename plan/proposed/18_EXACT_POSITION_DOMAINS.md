# Proposal 18: Exact, typed position domains and composable time maps

> **Status: EXECUTED 2026-07-14 (Phases 0-4)** — see `plan/STATE.md`.
> Phase 5's exact-invalidation primitive (`twLoopMap::preimagesWithin`)
> landed with Phase 4; wiring it into scoped invalidation, and the
> deep-nesting drift fixture, remain open.
> Follow-up to the 2026-07-12 split-clip-offset bug and the 2026-07-13/14
> stretch double-apply bug (commit `eaa1bec`); companion to
> `docs/EXACT_ARITHMETIC_DESIGN.md` (2025-06) and
> `docs/contracts/POSITION_DOMAINS.md`.

## Problem

Two independent failure classes keep producing timeline bugs, and they
compound each other:

**1. Domain confusion (correctness).** A raw `offset_t` does not say which
domain it lives in. Every position bug so far — the split-clip tail playing
from file start, the slipped+stretched clip playing a different window than
displayed, the looped+stretched clip repeating the wrong segment at the
wrong period — was a call site disagreeing with the storage convention about
whether a number was timeline, clip-relative, warped (grain-output), or
source domain. The last bug existed at four sibling sites simultaneously
(`rebuildReader`, `seekTo`, `mapTimelineToComponentPos`, `buildCapture_`)
because each hand-implemented the same conversion. `POSITION_DOMAINS.md`
documents the rules, but documentation is not enforcement.

**2. Rounding drift (exactness).** Positions and lengths are integers *by
nature* at their point of origin — a WAV frame count, a bar length at a
given tempo and rate, a snap-to-grid mouse position. They only become
inexact when a stretch factor passes through `double` and gets rounded back
per call site:

- `twGrainParams::stretch` is a `double`, although every producer computes
  it as a **ratio of two integer frame counts** (`newDur / srcSpan` in the
  stretch gesture) and every consumer multiplies a frame count by it and
  rounds (`llround` in the grain source, `+0.5` in the gesture's offset
  rescale, `llround` at the old scut.cpp sites).
- The stretch gesture *rescales* `startOffset` on every edit
  (`newOffset = oldOffset * newStretch / s0 + 0.5`): each re-stretch of the
  same clip rounds again. Stretch a clip five times and the source anchor
  has drifted by up to five half-frames — silently, and differently in the
  preview (which divides) than in a hypothetical exact chain.
- The `.qxp` stores `stretch='0.927635'` — six significant digits of a
  value that was born as an exact ratio. Load → save is not idempotent.

Why this matters more here than in a typical DAW: **we deliberately allow
and encourage re-using arrangement parts as source material** (container-
backed cuts render a capture that is itself windowed, stretched, and looped
by other cuts — proposal 10's recursive render cache). Each nesting level
multiplies a factor and rounds. Material may also be stretched several
times over its life (gesture → undo → different stretch → group broadcast
resize). Half-frame errors that are inaudible at depth 1 become audible
seam clicks and beat drift at depth 4, and undo/redo is no longer
guaranteed to restore the original bit-exactly.

`EXACT_ARITHMETIC_DESIGN.md` (June 2025) already commits to fractions for
all time values and factor composition down the tree ("like a graphics
transformation stack"). Phase 1 of it partially exists: `Fraction`
(tw/core/twfraction.h) + `parseFractionOrDouble`, used today for
*serializing* some SCut fields. This proposal is the missing execution
plan for the rest — plus the type-level enforcement that the June design
does not cover, and hardening the `Fraction` type itself, which is not yet
fit for position arithmetic (see Phase 0).

## Goals

1. A stored time value's **domain is machine-checked** — mixing domains or
   transforming a length like a point is a compile error, not a code-review
   hope.
2. Every domain conversion exists **exactly once**, as a first-class map
   object shared by preview, playback, freeze, and invalidation.
3. Time arithmetic is **exact end-to-end**: integer where integral by
   nature, rational where a factor is involved. Rounding to sample indices
   happens **exactly once**, at the render boundary, under a single
   documented rule.
4. Nesting depth and repeated stretching do not accumulate error:
   `stretch(a) ∘ stretch(b)` equals `stretch(a·b)` exactly, and
   do/undo/redo cycles restore bit-identical state.

## Non-goals

- No behavior change to the audio path's block/page architecture. Maps are
  composed per page, not evaluated per sample (hot-path cost stays what it
  is today: an add and a compare per block).
- No change to page-cache keying (component-own domain; rule 4 of
  POSITION_DOMAINS.md). The map lives at the `twView` seam, formalizing the
  current design.
- Amplitude, pan, pitch-cents stay floating point (they are not time).
- The grain synthesis *inside* a grain (window shapes, per-grain resample
  for pitch) stays floating point; only grain scheduling positions
  (hop/output length/window anchors) become exact.

## Design

### A. The value model: integers by nature, rationals by derivation

Store the quantities that are integral **at their origin**, derive the
rest exactly:

| Quantity | Nature | Representation |
|---|---|---|
| SLink startTime | timeline placement (snapped) | integer timeline frames |
| Source window anchor (`srcStart`, `srcSpan`) | positions in a concrete recording/capture | integer source frames |
| `cutDuration`, `loopLength` | musical timeline lengths (bars) | integer timeline frames |
| `stretch` | ratio of two frame counts | `Fraction` (n/d), created exact |
| Warped-domain values (today's `startOffset`) | **derived**: `srcStart · stretch` | `Fraction`, computed on demand, never stored |
| grain size / crossfade | engine params | integer frames (already) |

This inverts today's storage: `SCut` currently persists the *derived*
warped-domain `startOffset` and recomputes the source anchor by division.
Under this proposal the **source-domain window is authoritative** (it is
integral by nature — it points into real material), and warped/timeline
projections are exact rationals derived through the stretch factor.
Re-stretching then touches only the factor; the anchor never moves, so
repeated stretch edits cannot drift. This also makes the stretch gesture's
"source window invariant" rescale (`sstdmixerview.cpp`) a no-op instead of
a rounding site.

`stretch` is created as a fraction at the only places stretch is born:
- stretch gesture: `Fraction(newDur, srcSpan)` — both already integers;
- resize-clip action XML: parse `"3/2"` (exact) or legacy decimal
  (continued-fraction recovery, once, at load).

Composition of stretches (nested containers, re-stretch, group broadcast)
is fraction multiplication — exact by construction, matching
EXACT_ARITHMETIC_DESIGN.md's factor-aggregation model.

**Denominator growth.** Products of user-created ratios grow denominators.
Two containment rules: (1) gcd-normalize on every operation (already done);
(2) clamp at **creation** time, not during arithmetic — when a gesture
mints a stretch, snap it to a maximum denominator (e.g. 2^20) *before* it
enters the model. Approximating once at the moment of creation is exact
forever after; approximating during arithmetic would silently defeat the
whole scheme. Assert (debug) on denominators past a red-line to catch
pathological chains early.

### B. Typed domains (compile-time enforcement)

Strong wrapper types over the exact values, one per row of
POSITION_DOMAINS.md's table:

```
TimelinePos / TimelineLen      — integer frames @ project rate
ClipPos     / ClipLen          — integer frames, zero at clip start
WarpedPos   / WarpedLen        — Fraction frames (grain-output domain)
SourcePos   / SourceLen        — integer frames of a concrete source
```

Rules the types encode:
- No implicit cross-domain arithmetic. `ClipPos + WarpedLen` does not
  compile.
- **Points and lengths are distinct.** `Pos − Pos → Len`, `Pos + Len → Pos`,
  `Len ± Len → Len`; `Pos + Pos` does not compile. The loop bug —
  a length transformed like a point — becomes unrepresentable.
- Conversions are named functions in ONE header
  (`tw/core/twdomains.h`, next to the Fraction type), e.g.
  `warpedFromSource(SourcePos, stretch)`, `sourceFromWarped(WarpedPos,
  stretch)`. Grep-able, testable, single point of truth.
- `offset_t`/`length_t` remain the raw currency of the engine's inner
  loops and page keys; the typed layer lives where *windows are defined
  and transformed* (SCut, SLink, actions, gestures, renderers). The
  boundary where types unwrap to raw frames is exactly the boundary where
  rounding happens (see D).

Migration is mechanical and self-auditing: change one field's type, chase
the compile errors, each error site is a site that previously relied on
convention.

### C. twTimeMap: first-class, bidirectional, interval-aware maps

Promote `twView::MapPosFn` + `SObject::mapTimelineToComponentPos` from
point-only, forward-only closures to a small interface:

```cpp
struct twTimeMap {
    // exact endpoint mapping (Fraction in, Fraction out)
    virtual FracPos  map(FracPos parentPos) const = 0;
    virtual FracPos  inverse(FracPos childPos) const = 0;   // canonical preimage
    // interval mapping; may return multiple segments (loop tiling)
    virtual SegmentList mapInterval(FracPos start, FracLen len) const = 0;
    // full preimage of a child interval (invalidation wants ALL images)
    virtual SegmentList preimages(FracPos start, FracLen len) const = 0;
    // classification for fast paths / compile-per-page
    virtual bool isAffine() const = 0;   // scale + offset only
};
```

Concrete maps in the chain, each owning the ONE implementation of its
conversion:

- `ShiftMap` (SLink startTime; affine, scale 1)
- `WindowMap` (SCut slip: clip-relative → warped; affine, scale 1,
  offset = derived warped anchor)
- `StretchMap` (warped → source; affine, scale = 1/stretch as Fraction)
- `RateMap` (project rate ↔ native file rate seam; affine — the ratio of
  two integer sample rates is exact by nature)
- `LoopMap` (clip-relative → warped with tiling; **non-affine**: exact
  modular arithmetic; `mapInterval` returns the ≤N wrap segments —
  today's chunking loop in `twLoopReader::calcOutputTo`, extracted;
  `preimages` returns every timeline image of a source interval)

Composition walks the parent chain like a transformation stack. Affine
runs compose symbolically into a single `{scale: Fraction, offset:
Fraction}` — exact, and cheap to evaluate. A `LoopMap` in the chain splits
composition into per-segment affine pieces.

**Consumers — the important part:**
- `twView::seekTo/freezePage/freezePreviewPage` use the map (as today,
  formalized).
- `SCutRendererInline` (waveform preview) uses the **same map objects** —
  the "display right, audio wrong" divergence class becomes structurally
  impossible, because there is no second implementation to disagree.
- Scoped invalidation can move from "bump the whole component" to exact
  `preimages()` of an edited source interval (optional refinement; epochs
  stay as the safety net).

**Non-affine future (the user-raised point).** Components that are not
affine projections of the time interval fit the same interface as long as
they are *piecewise-affine with rational breakpoints* — which covers loop
tiling today and an automated-stretch warp curve tomorrow (a warp curve
edited as breakpoints IS piecewise-affine by construction). Maps that are
genuinely non-algebraic (a freeform tempo curve sampled from a performance)
would define a documented exactness boundary: exact breakpoints, defined
interpolation rule, single rounding at render — the framework marks where
exactness ends instead of pretending it doesn't.

### D. One rounding rule, at one boundary

Exact rationals must become integer sample indices exactly once — at the
render/materialization boundary (page fill, grain buffer build, preview
peak binning). The rule:

- **Interval starts round by floor;** interval **ends are exclusive** and
  derived as `floor(start + len)` in the exact domain — never as
  `floor(start) + floor(len)`.
- Consequence: adjacent exact intervals tile the sample grid with **no
  gaps and no overlaps** (the classic source of seam clicks and off-by-one
  bleed), and `Σ rounded-segment-lengths == rounded-total-length` by
  construction.
- `twGrainSource`'s output length becomes `floor(inLen · stretch)` with
  the fraction — replacing today's `llround(inLen * (double)stretch)`,
  whose half-frame ambiguity currently decides whether a loop's last
  frame exists.

### Phase 0 (prerequisite): harden `Fraction`

The current `tw/core/twfraction.h` is **not safe for position arithmetic**:

- `uint64_t` numerator — **cannot represent negative values**; worse,
  `operator-` silently **clamps to zero**. Position deltas and leftward
  slips would corrupt silently. → signed `int64_t` numerator, unsigned
  denominator, no clamping.
- `a*d + b*c` style intermediates overflow 64 bits long before the
  representable range does. → 128-bit intermediates (`__int128` on our
  toolchains) or checked multiply with pre-normalization
  (`gcd`-reduce cross terms before multiplying).
- Comparison operators have the same overflow hazard.
- Keep `toDouble()` strictly for display/debug (grep-able name:
  `approxDouble()`), so accidental exactness leaks are searchable.

Extend `tw303a/core/tests/test_exact_arithmetic.cpp` with property tests:
sign round-trips, `(a−b)+b == a`, associativity of composition, overflow
regression cases, `map(inverse(x)) == x` for every map type.

## Phases

Each phase is independently shippable; the qxa suite (25/25, run from
`tests/cases/`) must stay green after every phase.

- **Phase 0 — Fraction hardening.** Signed, overflow-safe, tested. No
  callers change behavior.
- **Phase 1 — Typed domains in the clip layer.** Wrap SCut window fields,
  SLink startTime, resize/split/place action payloads, and the gesture
  math in domain types. Pure re-typing; the compiler produces the audit
  trail. (This alone would have made both shipped bugs compile errors.)
- **Phase 2 — Rational stretch end-to-end.** `twGrainParams.stretch`
  becomes `Fraction`; gestures mint ratios of integers; `.qxp` writes
  `stretch='n/d'` (parser already accepts both ways); grain output length
  via exact floor. Legacy decimals recovered once at load via the existing
  continued-fraction path.
- **Phase 3 — Source-domain authoritative window.** SCut persists
  `srcStart/srcSpan` (integers) + stretch; warped values derived. Loader
  migrates legacy warped `startOffset` by exact division (result may be
  rational — that is precisely the legacy rounding made visible; keep it
  as the exact anchor rather than re-rounding). Gesture rescale sites
  deleted. Verification: stretch→unstretch returns bit-identical
  serialization; 100× do/undo/redo idempotent; save→load→save fixpoint.
- **Phase 4 — twTimeMap.** Extract the five maps, compose exactly,
  compile-per-page for the audio path, preview and playback share
  instances. `twLoopReader`'s chunk loop becomes `LoopMap::mapInterval`.
- **Phase 5 (optional) — exact invalidation preimages** and warp-curve
  groundwork (piecewise-affine maps with rational breakpoints).

## Verification strategy (beyond per-phase notes)

- **Nesting-drift test (the motivating property):** build a fixture
  arrangement N levels deep (capture of a stretched cut of a capture of a
  stretched cut …, factors chosen so the product is 1/1). Render at each
  depth; assert the depth-N render is **sample-exact** against depth-0.
  Today this fails at some depth; after Phase 3+4 it must pass for any N
  until the denominator red-line.
- **Preview/playback agreement:** a qxa-level check that windowed preview
  peaks and rendered audio agree on which source region is shown/played
  (generalizing `grain_loop_stretch.qxa` from one instance to a property).
- **Tiling property:** for random exact windows, the rounded segments of
  `mapInterval` tile `[floor(start), floor(start+len))` with no gap or
  overlap.

## Open decisions

1. **Loop anchor domain:** keep `loopLength` integral in timeline frames
   (musical quantity, snapped) — proposed — or make it source-domain like
   the window? Timeline keeps the bar-loop use case exact at any stretch.
2. **Denominator cap at creation:** value (2^20?) and what the UI does at
   the red-line (refuse? snap?).
3. **How far down do typed positions go?** Proposal: stop at the reader
   seam (`twView`/page keys stay raw `offset_t`); revisit after Phase 4.
4. **Display:** UI keeps showing seconds/bars (decimal); fractions are an
   internal + file-format representation only (matches
   EXACT_ARITHMETIC_DESIGN.md open item 2).
5. **`posFactor` per-level aggregation** (EXACT_ARITHMETIC_DESIGN.md §2)
   vs. per-object stretch fields: this proposal keeps per-object factors
   and composes them in twTimeMap; the file-format `posFactor` hierarchy
   remains a serialization-level convention. Unify later if the event
   timelines work (proposal 12) needs it.
