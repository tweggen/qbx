# Proposal 25 — Render transform pushdown: compose pitch/stretch/gain to the leaf, granulate once

> **Status: DRAFT (2026-07-23).** Architectural design — no code yet.
> Prerequisite reading: `06` (grain source), `18_EXACT_POSITION_DOMAINS.md`
> (twAffineMap / twTimeMap / typed positions), `19_ASYNC_FREEZE_MODEL.md`
> ("Phase 2 REVISED" — planPage/freezePage, the page cache and epochs),
> `22_CLIP_PITCH.md` (the grain stage this reuses).

## Motivation

Granulating an already-granulated signal produces heavy artifacts. Reported
in use: a 4-bar asset whose bars 2–4 are grain-pitched copies of a recorded
bar 1, doubled onto another track an octave up. The octave was so degraded it
was barely recognizable.

This is technically correct for today's engine. A container-backed `SCut` (an
"asset" — content is a track/mixer, not a wave) freezes the container to a PCM
**capture** and then applies the outer cut's grain **over that capture**
(`main/objects/cut/src/scut.cpp:156`, whose input `*rs` is the capture built at
`scut.cpp:262-396`). The capture already contains the child bars' grain (baked
during the freeze), so the octave granulates grain — two generations.

What a DAW user instinctively expects: pitch, stretch and volume behave like
**parameters that pass down** the tree, and grain synthesis happens **once**, at
the leaf sample, over raw material, at the composed value (`child_pitch +
octave`). This proposal makes that the engine's behavior.

Decisions taken with the requester: implement the **full** transform (pitch +
stretch + gain), and **propagate by default** — a clean octave with no knob to
discover. Delivery is staged for verifiability, but the end target is all three
axes, on by default.

## Key insight

Pitch, gain and stretch **distribute over a sum**: `octave(a+b+c+d) ≈
octave(a)+octave(b)+octave(c)+octave(d)` for a resampler, and `gain·Σ = Σ gain·`
exactly. So a transform applied to a container's output can be pushed into each
child, composed with the child's own transform, and realized once per leaf —
cleaner *and* higher fidelity than granulating a pre-mixed, pre-granulated
capture. The distribution is valid only through **linear** nodes (sums, gains);
a plugin insert is nonlinear and is the hard stop.

Every primitive this needs already exists and is exact:

- `twAffineMap::composedWith` (`tw303a/core/include/tw/core/twtimemap.h:73-76`) —
  exact rational map composition (`scale*scale`, `offset*scale+offset`).
  **Currently only exercised by a test**, never in the render chain.
- `Fraction` exact 128-bit arithmetic for multiplied stretch; additive
  `pitchCents`; multiplicative gain (`twgrainparams.h:21-33`).
- `twGrainSource` already materializes a warped buffer once in its ctor and is
  documented "built once, cacheable, **shareable across cuts**" (proposal 06
  §7.2, `twgrainsource.h:14-18`) — the per-variant leaf cache below is exactly
  what that anticipated.
- The tree-walk that carries a composed map across container boundaries also
  already exists — but only in the **invalidation** path
  (`SObject::invalidateRenderChainsContainingRange` +
  `SCut::mapChildRangesToSelf`, which folds stretch/slip/loop, `scut.cpp:668-706`).
  This proposal gives the **render** path the composition the invalidation path
  already has.

## The transform

New `tw303a/core/include/tw/core/twrendertransform.h`, beside `twtimemap.h`:

```cpp
struct twRenderTransform {
    double      pitchCents = 0.0;                  // additive
    Fraction    stretch    = Fraction(1);          // multiplicative
    double      gain       = 1.0;                  // multiplicative linear
    twAffineMap posMap{Fraction(1), Fraction(0)};  // identity until Phase 2
    bool     isIdentity() const;
    twRenderTransform composedWith(const twRenderTransform& inner) const; // outer ∘ inner
    uint64_t hash() const;                         // cache-key dimension (canonicalized FP)
};
```

`composedWith`: `pitch = pitch + inner.pitch`, `stretch = stretch *
inner.stretch`, `gain = gain * inner.gain`, `posMap = posMap.composedWith(
inner.posMap)`. The leaf builds its grain at `effective = inherited ∘ own
grainParams`.

Grain **engine** knobs (`grainSize`, `crossfade`) do NOT compose — the leaf
granulates with its own, since that is where granulation happens. The transform
carries only pitch/stretch/gain. Documented behavior change: an outer clip's
grain-size knob no longer affects a propagated render.

## The pushdown rule

`freezePage`/`planPage` gain an inherited `const twRenderTransform& xf = {}`
(identity default, so every untouched call site stays byte-exact). A new virtual
`twComponent::distributesTransform() const` (default `false`) marks linear nodes.

- **Linear summing node** (`twTrackMix`, `twMixer`, `twRewire` → `true`):
  forwards `xf` to each child's freeze; sums the already-transformed children.
  The node's **own** gain stays post-sum (`twtrackmix.cc:484-490`) — only the
  *inherited* transform distributes, so no `a(x+y)` vs `ax+ay` last-bit drift.
- **Leaf sample cut** (`SCut` over a wave): builds one `twGrainSource` at
  `effective` — `stretch*=`, `pitch+=`, `gain` as a post-read scalar. Each raw
  sample granulated **once**. This is the fix: the octave folds into each bar's
  pitch in cents.
- **Container cut** (`SCut` over a track): becomes a **propagator** — resolves to
  the container's linear entry (`rewire → pluginChain(bypassed) → trackmix`, all
  `distributesTransform()`), forwards `xf ∘ own grain` down. No capture, no
  grain-over-capture.
- **Nonlinear node** (`twPluginInsert::process`, `twplugininsert.cc:242`; a
  non-bypassed `twPluginChain` → `false`): the hard stop. It freezes its input at
  **identity** and the transform is realized as a grain stage **above** it — i.e.
  today's `buildCapture_`+grain path, retained **verbatim** and scoped to exactly
  this case. A track with an insert re-grains as today; a plugin-free asset (the
  reported case) distributes to the leaves.

`distributesTransform()` is structural, so linearity is decided cheaply at
reader-build time and re-checked when inserts are added/bypassed (invalidate the
reader).

## Cache key — by transform VALUE

Pages key on `(component*, pageStart)` + `contentEpoch` today
(`capture_revalidator.h:305`, `twcomponent.h:538`); two transforms on one
component would alias. Add `xfHash` to the scheduler `NodeKey` and the
per-component `outputPages_`/`inflight_` keys.

Keying by transform **value** (not placement identity):

- A shared asset placed at +0 / +1200 / −1200 yields three independent renders,
  no aliasing (today, with no transform in the key, they would silently alias).
- Two placements at the same effective transform dedup to one grain buffer + one
  page set.
- `contentEpoch` stays orthogonal: a raw-sample edit bumps the component epoch
  and **every** transform-variant page goes stale together — correct, since a
  raw edit dirties every transposition. Invalidation needs **zero** transform
  awareness. This is the cleanest property of the design.

## Preview stays on its capture

`buildCapture_` and `getPreview` are unchanged, now built at **identity** — the
clip's own waveform in its own lane, which is exactly what a self-preview should
show (the parent's transposition is not part of this clip's preview). Only the
*playback* reader path for linear container cuts switches to propagation. Clean
split: preview = self-capture, render = propagate.

## Invariants preserved

- **RT never renders.** Freeze/plan already run only off-RT (workers, offline).
  The transform changes *what* is rendered, not *who*. Identity variant = the
  existing `currentReader_` (RT fast path at `scut.cpp:215-219` intact).
  Non-identity variants are built only in freeze context; an RT read that finds
  no identity variant serves stale/silence as today — never builds.
- **Epoch staleness** is per-component and per-variant; the
  `contentEpoch >= epochNow` gate (`twcomponent.cc:544-545`) is unchanged.

## Touch points (representative)

- **New:** `tw303a/core/include/tw/core/twrendertransform.h`.
- **Structs:** `tw303a/graph/include/tw/graph/tw_page_plan.h:34-44` — add the
  transform to `twPageDep` and `twPagePlan`.
- **Graph:** `twcomponent.h/.cc` — `xf` on `freezePage` / `requestPage` /
  `freezePageWithInputs` / `freezePageFromInputs` / `freezePage_nolock` /
  `freezePreviewPage` / `planPage`; the `distributesTransform()` virtual; extend
  `outputPages_`/`inflight_` keys and serve/stamp (`twcomponent.cc:500-648`).
- **Mix:** `twtrackmix.cc:360-398` (planPage forwards composed `xf` per dep),
  `:401-495` (freezePage forwards `xf`, own gain post-sum); `twmixer.cc:67-127`;
  `twrewire` → `distributesTransform()==true`.
- **View:** `tw303a/graph/src/twview.cc:14-22,64-86` — Phase 1 pass `xf` through
  (identity `posMap`); Phase 2 compose `posMap` into `mappedPos` and child `xf`.
- **Cut:** `main/objects/cut/src/scut.cpp` — propagator path for linear
  container subtrees; per-effective-transform reader-variant cache
  (`map<xfHash,{grain,reader}>`, LRU, identity = `currentReader_`); retain
  `buildCapture_`+grain (`:156`, `:262-396`) as the nonlinear fallback;
  `resolveClip` (`:644-659`) threads `xf`.
- **Plugins:** `twpluginchain.cc` (`distributesTransform()` = no active insert),
  `twplugininsert.cc` (= `bypass_`).
- **Scheduler:** `capture_revalidator.h:305-306` / `.cc:283-288,344-387` — carry
  `xfHash` through `NodeKey`, `expandNode_`, `processGraphNode`.

## Phasing

- **Phase 0 — plumbing, no behavior change.** Add the type,
  `distributesTransform`, thread identity-defaulted `xf` everywhere, extend cache
  keys. Gate: byte-exact identity render.
- **Phase 1 — pitch + gain pushdown.** `posMap` stays identity (position-
  invariant: `twgrainsource.h:20-22` — pitch preserves time). Ships the reported
  fix. Propagate-by-default for plugin-free container subtrees.
- **Phase 2 — stretch.** Turn on `posMap` composition in `twView::resolve` /
  `freezePage`; handle page-boundary re-alignment, clip-duration clamping
  (`twtrackmix.cc:471-481`), and `twLoopMap` tiling interaction. The genuinely
  invasive part; staged so Phase 1's fix ships independently.

## Verification (qxa, run from `smaragd/tests/cases/`, fixture `../test_sawtooth.wav`)

- **Byte-exact identity gate (release gate).** Every project with no nested
  transform → rendered WAV hash equals its pre-change baseline. Forces the
  identity path to be a strict no-op (default-arg threading, no reassociation of
  the post-sum gain). Kept green after every phase.
- **Bug repro.** 4-bar asset, bars 2–4 grain-pitched copies of bar 1, placed an
  octave up. Assert per-source-second RMS **and** HF/spectral energy against a
  hand-built single-generation reference (child bars pre-composed to
  `child_pitch + octave`, granulated once); assert HF artifact energy strictly
  below the old grain-over-grain render.
- **Gain distribution exactness.** Nested track gain: pushed-down leaf gain vs
  post-sum gain differ < FP epsilon.
- **Plugin hard stop.** Asset track with a non-bypassed insert, placed octave up:
  assert propagation is NOT taken and output = grain(pluginOutput, octave).
- **Shared asset, N placements.** +0 / +1200 / −1200 → three distinct outputs,
  three cached buffers, no aliasing; two identical effective transforms → one
  shared buffer.
- **Phase 2 stretch.** Nested stretch composes multiplicatively; assert output
  duration and sample alignment vs a single-generation reference; loop-tiled clip
  under a parent stretch tiles correctly.
- Each phase: full `ctest` green; `check_layering.py` / `check_logging.py` clean.

## Risks

- **Memory:** one resident `twGrainSource` buffer per **distinct** effective
  param at a leaf. Mitigate with an LRU/refcounted variant cache evicted when no
  live page references it; value-dedup bounds the common case.
- **grain-over-sum ≈ sum-of-grains (pitch only).** Distributing pitch through a
  sum then granulating each child is not bit-identical to granulating the summed
  signal — but is strictly better than grain-over-grain (single generation over
  raw material). Gain distributes exactly. Gate on artifact-level RMS/HF vs a
  single-generation reference, **not** bit-equality, and deliberately keep **no**
  gate against the old nested output (that was the bug).
- **Scope:** Phase 2 (stretch) touches the position-remap layer that interacts
  with loop tiling, duration clamping and page alignment — the invasive part.

## Follow-ups this enables / relates to

- A dedicated `twTransformRealizer` wrapper could later replace the SCut-local
  nonlinear fallback for the track-in-track (folder) group case, where the stop
  is inside the graph rather than at an SCut boundary.
- Pre-existing bug surfaced nearby (not part of this proposal): duplicating a
  container-backed asset mis-renders its window (`SDuplicateClipAction` /
  container capture); tracked separately in `STATE.md`.
