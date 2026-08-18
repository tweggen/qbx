# Proposal 39 — The folder lane's sum waveform, and preview/volume decoupling

> **Status: PROPOSED (2026-08-18).** Two changes, deliberately in one run: they
> touch the same eighteen lines of `drawObjectWaveform` and the same semantic
> question — *what does a drawn waveform describe?*

Prerequisite reading: `smaragd/main/timeline/CONTRACT.md` (inv. 1, 2, 5, 10),
`smaragd/main/objects/wave/CONTRACT.md` (the channel fold),
`plan/proposed/36_MULTICHANNEL_SIGNAL_FLOW.md` §4.3-§4.6,
`plan/proposed/34_LEVEL_METERS.md` (why a level lives where it lives),
`smaragd/main/testkit/src/preview_container_test.cpp` (header comment).

## Why

**1. A folder lane is a blank rectangle.** A track that has child tracks is a
summing container (`strack.h:304-312`) whose lane paints one flat colour and
returns — `STrackRendererInline::draw()` fills the lane
(`strackrndrinline.cpp:38-45`), then `continue`s past every child track in the
clip loop (`:52-56`, and correctly so: a child is its own lane). Collapse the
folder and the arrangement underneath it disappears from the screen entirely.
There is nothing in the arranger that says *this folder has material here*.

**2. A clip's waveform is scaled by the fader of the lane it happens to sit
on.** `drawObjectWaveform` reads the containing track's fader at paint time and
multiplies the preview probes by it:

```cpp
// swaveformdraw.cpp:36-53
double volumeGain = 1.0;
if( SObject *parentObj = dynamic_cast<SObject *>( lk.parent() ) )
    volumeGain = pow( 10.0, parentObj->volumeDbSnapshot() / 20.0 );
...
int scaledMin = (int)( pv[i].min * volumeGain );
int scaledMax = (int)( pv[i].max * volumeGain );
```

`lk.parent()` is the containing `STrack` in every path that creates a clip link
(`saddsampleaction.cpp:84`, `splaceclipaction.cpp:50`, `ssplitclipaction.cpp:91`
and eleven more). So pulling a fader down redraws every clip on that lane as a
thinner waveform, and at −40 dB the arrangement visually empties. That is wrong
for three independent reasons:

- **A waveform is the content, a fader is the level.** Every reference DAW draws
  a clip at its recorded amplitude and shows the fader on the fader. We have a
  fader widget *and* a level meter (proposal 34) — the drawn waveform is the one
  place level does not belong.
- **It is quantised to 8-bit probes.** `pv[i]` is `signed char`
  (`twtypes.h:33-37`), so the multiply happens *after* quantisation: at −20 dB
  the whole envelope collapses onto about a dozen distinct values and the clip
  draws as a coarse ladder, not as a quieter version of itself.
- **It is not what the stored bytes mean.** `plan/STATE.md:6576-6580` already
  records that volume is *not* baked into stored preview bytes — the paint-time
  multiply is the entire dependency, and it contradicts the sidecar's own
  contract.

## The rule this proposal adopts

> **A drawn waveform describes the audio its object PRODUCES. The lane it is
> drawn on never scales it.**

One sentence, and it decides both halves:

- A clip's preview loses the containing track's fader (change 2).
- A folder's background sum is built from its children **without** the folder's
  own fader, mute or inserts anywhere in it (change 1) — because the folder is
  the lane being drawn on.
- A child one level down is *not* the lane being drawn on, so **a child's own
  fader does scale its contribution** (requester's decision). The recursive
  spelling: a descendant clip's contribution is scaled by the product of the
  gains of every track on the path from its own track up to **but excluding**
  the folder being drawn.
- An **asset** clip (a container-backed `SCut` referencing another track) keeps
  the referenced track's fader, because that fader is baked into the capture the
  preview is computed from (`SCut::buildCapture_` → `getRootComponent()` =
  `cpRewire_`, post-`twGainStage`, `strack.cpp:434-437, 817-821`). The
  referenced track is the clip's *content*, not its container, so the rule is
  satisfied. Stripping it too would mean freezing from `cpDspChain_` instead —
  a separate decision, recorded under "Deliberately not done".

## The headline finding: no engine changes are needed, and no freeze

The obvious design for change 1 is **wrong, and it is wrong in the way this
codebase has been wrong three times before** (the meters, MIDI-out, the
metronome): compute the folder's summed audio and draw that.

The plumbing to do it exists and is fully wired. `STrack` has no
`getRandomSource()`, `hasDuration()` is unconditionally true
(`strack.cpp:366-369`) and `getRootComponent()` is the rewire fed by the summing
`twTrackMix` — so `folderTrack->getPreview(...)` **already works today** and
already returns the folder's summed envelope, through
`SObject::straightCalcPreviewData()`'s container branch
(`sobject.cpp:456-510`). Nothing in the arranger calls it, and nothing should:

- The container branch reaches its pages through **`requestPage()`**
  (`sobject.cpp:481`), which **demands a freeze**. Calling it from `paintEvent`
  renders the folder on the UI thread — `main/timeline/CONTRACT.md` inv. 1
  forbids exactly that ("paint paths never block").
- Doing it off the paint thread instead means a background worker, a published
  snapshot, and an invalidation protocol for every child edit, gain change and
  plugin change — none of which currently invalidate `previewData_`, which is
  freed only on `durationChanged` / `gotUnreferenced` (`sobject.cpp:914-921`).
- And it is post-fader, so it would violate the rule above and go blank when the
  user pulls the folder's own fader down.

**The overlay is therefore built from the children's EXISTING previews**, which
are already computed, already cached and already drawn on the children's own
lanes. Per pixel column: sum the mins, sum the maxes, clamp. No engine edit, no
freeze, no worker, no cache, no invalidation, and it draws immediately on a
project that has never been played.

Its one honest limitation, stated in the CONTRACT and in the lane's tooltip:
**it is a sum of envelopes, not the envelope of a sum.** It over-states where
children are out of phase, and it does not see child plugins, instruments or
automation — those live only in frozen pages. It is a *hint about where material
is*, not a meter and not an oracle.

## Design

### D1 — one envelope walk, two terminal operations

`drawObjectWaveform` currently does three things in one function: map pixels to
the object's domain, fetch probes, and draw lines. Split the last one off.

```
// app/objects/wave/swaveformdraw.h
bool collectObjectEnvelope( SObject &obj, SLink &lk, SRenderContext &ctx,
                            preview_t *out, int w );   // NEW: fills w probes
bool drawObjectWaveform  ( SObject &obj, SLink &lk, SRenderContext &ctx,
                           const QColor &c );          // collect, then draw
```

`drawObjectWaveform` keeps its signature and every call site
(`splainwaverndrinline.cpp:44`, `srecordingrndrinline.cpp:16`,
`scutrndrinline.cpp:339`) — it becomes collect + draw.

**The volume multiply stays in the draw half through M1 and is deleted in M2**,
deliberately: that makes M1 a pure refactor whose acceptance criterion is *no
pixel and no byte moved*, and it means the M2 gate (probes byte-identical across
a fader change) is measuring the deletion rather than the refactor. It is never
reintroduced in the collect half.

**The collect API must not require a `QPainter`.** `SRenderContext` holds a
`QPainter&`, so a headless caller — the testkit verb, and any later non-paint
consumer — cannot build one honestly, and a dummy painter over a scratch
`QImage` is the kind of prop that hides a bug later. The window is expressed as
`{leftTime, rightTime, width}` and `drawObjectWaveform` derives it from
`ctx.getTimeOf()` exactly as it does today.

### D2 — the renderer seam, so the canvas never dynamic_casts

`main/timeline/CONTRACT.md` inv. 2 forbids branching on concrete object types in
the canvas: "where the canvas must branch on WHAT a clip is, it asks
`SObject::contentKind()`". A folder walker that reached into `SCut` to redo its
loop tiling and stretch mapping would be a fourth such violation, and would
silently disagree with the drawn clip the moment either changed.

So the walk goes through the renderer:

```
// SObjectRenderer (app/model/srendercontext.h)
virtual bool collectEnvelope( SLink &lk, SRenderContext &ctx,
                              preview_t *out, int w ) { return false; }
```

Implemented by exactly the renderers that draw waveforms today, each by routing
its **existing** segment walk to the collect terminal instead of the draw
terminal:

| Renderer | Today | After |
|---|---|---|
| `SPlainWaveRendererInline` | `drawObjectWaveform(...)` `:44` | `collectObjectEnvelope(...)` |
| `SRecordingRendererInline` | `drawObjectWaveform(...)` `:16` | same |
| `SCutRendererInline` | `drawSeg` lambda, container + loop tiling `:337-352` | the same lambda with a collect terminal; loop segments accumulate into their own pixel span |
| `STakeStackRendererInline` | delegates to the active take `stakestack.cpp:396` | delegates identically |

A renderer that does not implement it (an event clip) returns false and
contributes nothing — an event clip has no waveform, which is the right answer,
not a bug.

### D3 — the folder walk

New, in `main/objects/track/` (it is track knowledge, and `objects/track` may
not depend on `timeline`):

```
// STrack
bool collectChildSumEnvelope( SRenderContext &ctx, preview_t *out, int w ) const;
bool hasChildTracks() const;
```

- Walk `childLinks()`; a link whose object is an `STrack` is a child track.
  (This is the same predicate the arranger already spells locally as
  `hasChildTracks()` in `sstdmixerview.cpp:2916-2924` — that copy is replaced by
  the model one, so there is one definition.)
- Recurse into nested folders, carrying an accumulated linear gain.
- **Audibility comes from `ssolo::isLaneAudible`** (`app/model/ssolorules.h`),
  never re-spelled here — `main/timeline/CONTRACT.md` inv. 10 records that the
  two meter call sites' local copies of the direct-children-only rule are how
  the meter and the ear came to disagree about a nested lane. An inaudible lane
  contributes nothing.
- For each clip link on each such track, ask its renderer for `w` probes over
  the SAME visible time range, scale by that lane's accumulated gain, and
  accumulate `sumMin += min`, `sumMax += max` per column into `int32` before
  one clamp to `[-127, 127]` at the end. Accumulating in `preview_t` would wrap
  a `signed char` on the second loud child and draw the sum as a *quieter*
  waveform than one child.
- Return false if nothing contributed, so the painter draws nothing at all —
  which is what "at least when they are non-zero" asks for.

### D4 — where it is drawn, and in what colour

Inside `STrackRendererInline::draw()`, immediately after the lane fill
(`strackrndrinline.cpp:45`) and before the clip loop, so it sits **behind** the
folder's own clips and above the lane background. Not in `sstdmixerview.cpp`:
an overlay drawn at the canvas's own seam (`:353-364`) would be painted over by
the renderer's own full-rect fill on the very next line.

The early `if( getTrack().isEmpty() ) return;` (`:46-50`) must move **below**
the overlay, or a folder holding only child tracks (the common case) returns
before drawing anything.

Colour: `finalColor` lightened — `finalColor.lighter(140)` at ~55 % alpha, i.e.
derived from the lane's own final colour so it tracks selection and the
mute/solo/armed modifiers (`STrackColorModifier`) instead of being a fourth
hardcoded constant. "A bit lighter than the background" is the requirement; the
gate asserts the luminance relation rather than the constant.

### D5 — cost, and why it needs no cache

Per repaint, per folder lane: (visible clips in the subtree) × (lane width in
pixels) probe lookups. `SObject::getStraightPreview` is an index into an
already-built array (`sobject.cpp:591-592`); the array is built once per object
and reused. That is the same order of work the arranger already does to draw
those same clips on their own lanes when the folder is expanded — the overlay
makes a *collapsed* folder cost about what an expanded one costs. No cache, no
snapshot, no invalidation, no thread.

The one path that can be expensive is the **first** `straightCalcPreviewData()`
for a container-backed (asset) child, which freezes pages. That risk exists on
today's paint path already and is not created here; the folder walk does not add
a new one, and M4's gate records the measurement rather than asserting a bound.

## Milestones, gates and acceptance criteria

Each milestone is a gate: it does not close until every AC under it is
demonstrated, and the evidence goes in the PR body. Gates run **from
`smaragd/tests/cases/`** (CWD-relative fixtures) after `./build.sh`, which
re-configures — the qxa glob is `CONFIGURE_DEPENDS` and a new case is invisible
without it.

### M0 — Baseline (no code)

| AC | Criterion |
|---|---|
| M0.1 | The worktree builds clean: `./build.sh` green, `python3 tools/check_layering.py` and `python3 tools/check_logging.py` clean. |
| M0.2 | `ctest --test-dir smaragd/build -j4 --output-on-failure` green, and the count reconciled against `ctest -N` (measured on this branch point: **200 registered**; CLAUDE.md's 174 predates later cases). Any pre-existing failure is named in the PR as pre-existing, with `repeat_test.sh` evidence. |
| M0.3 | A baseline render from a committed golden case is kept for the byte gate in M4.4. |

### M1 — The collect seam, and the envelope verb

| AC | Criterion |
|---|---|
| M1.1 | `collectObjectEnvelope()` exists and `drawObjectWaveform()` is implemented in terms of it; the inline renderers gain `collectEnvelope()` overrides; `SObjectRenderer::collectEnvelope` defaults to `false`. |
| M1.2 | **Collect and draw cannot drift**: `drawObjectWaveform` has exactly one probe-producing path, and it is `collectObjectEnvelope`. Demonstrated by the diff and by AC M1.3. |
| M1.3 | New C++ gate `preview_envelope_test` (ctest), a sibling of `preview_container_test`: for a plain wave, an `SCut` with a slip offset, and a LOOPED `SCut`, the probes returned by `collectEnvelope()` equal the probes the renderer draws, at page boundaries and inside loop tiles. A loop tile is where a naive collect implementation silently returns tile 0 for every tile. |
| M1.4 | An object whose renderer does not implement it (an event clip) returns false and writes nothing to the output buffer — asserted, not assumed. |
| M1.5 | The testkit verb `assert-envelope` exists and is the ONE way a script reads a drawn envelope. Two modes on one verb: `clip="0,0"` (a clip's own probes, through its renderer) and `trackPath="0" mode="childSum"` (M3's folder walk). Attributes: `start`, `length`, `width`, `column`, `min`/`max`, `tolerance`, `expectEmpty`, plus `snapshot="<name>"` / `compareTo="<name>"` which stores a probe array under a name and later asserts a LATER array is **byte-identical** to it. That pair is what makes "the fader did not move the waveform" assertable without hard-coding a single expected byte. It goes out through `SMainWindow` — testkit may not include `app/timeline`. |
| M1.6 | The verb is registered in `action_roundtrip_test` (write → read → write is stable) and documented in `docs/ACTIONS.md`. |
| M1.7 | `check_layering` and `check_logging` clean; the full suite still green and count-reconciled. |

### M2 — Preview/volume decoupling

| AC | Criterion |
|---|---|
| M2.1 | `swaveformdraw.cpp` contains no `volumeDbSnapshot`, no `pow( 10.0, … / 20.0 )` and no per-probe gain multiply. Gate: `grep -n "volumeDbSnapshot\|volumeGain" main/objects/wave/src/swaveformdraw.cpp` is EMPTY. |
| M2.2 | No other paint path reintroduces it: `grep -rn "volumeDbSnapshot" main/` hits only `sobject.h`/`sobject.cpp` (the accessor and its definition). |
| M2.3 | **A PIXEL gate, and it had to be.** The obvious gate — a qxa case that snapshots a clip's envelope, moves the fader and compares — **cannot bite, and the reason is structural**: `assert-envelope` reads through `collectEnvelope`, and M1 deliberately left the multiply in the DRAW half, so everything the verb can reach has been volume-independent since M1 landed. `preview_envelope_test` section 5 therefore paints through a link whose parent holds a non-unity fader and recovers the probes from the PIXELS (the `y = 127 - value` read-back M1 already built). Demonstrated failing pre-deletion at −20 / −6 / +6 / −60 dB, e.g. `painted -2/2, collected -20/20` at −60 dB `painted 0/0`. |
| M2.3a | `preview_volume_independent.qxa` is committed too, and its header states plainly that it PASSED pre-deletion and is not what caught the bug. It earns its place by stating the rule where a reader will look for it and by pinning it for every later consumer of the seam — M3's folder walk is one — not as evidence. |
| M2.4 | The same case asserts the envelope is likewise unchanged by `set-track-mute`. |
| M2.5 | `SObject::setVolume()`'s `invalidatePreview()`: **KEPT, comment rewritten.** Verified against the code — `straightCalcPreviewData`'s no-random-source branch reads `getRootComponent()`'s pages and `STrack::getRootComponent()` is `cpRewire_`, post-`twGainStage`, so a CONTAINER's own preview genuinely is post-fader and must be invalidated. Dropping the call would leave a folder or asset waveform drawing at the old gain until `durationChanged` happened to free `previewData_`. The old comment's claim was false for sample-backed objects and is now cited against `plan/STATE.md:6576-6580`. |
| M2.5a | `volumeDbSnapshot()` now has **zero callers** and is deliberately kept — it holds `volumeMutex_` where the bare `getVolume()` does not, and deleting it is a separate decision. `main/model/CONTRACT.md` records that, and no longer advertises the renderer that used it. |
| M2.6 | No golden moves: the committed render goldens are byte-identical (`cmp`). A paint change may not touch audio, and if it does, that is the finding. |

### M3 — The folder sum overlay

| AC | Criterion |
|---|---|
| M3.1 | `STrack::hasChildTracks()` exists and `sstdmixerview.cpp:2916-2924`'s local copy is deleted in favour of it — one definition. |
| M3.2 | `STrack::collectChildSumEnvelope()` accumulates in `int32` and clamps ONCE at the end. Gate: two children each at full scale sum to the clamp, never to a wrapped small value (asserted in M3.5). |
| M3.3 | Audibility is `ssolo::isLaneAudible`, not a local rule. Gate: `grep -n "isLaneAudible"` hits the new walk; no new `isMuted()`/`isSoloed()` chain appears beside it. |
| M3.4 | Each descendant's contribution is scaled by the product of the gains from its own track up to but EXCLUDING the folder (gated by M3.6 and M3.11). |
| M3.5 | `assert-envelope mode="childSum"` (M1.5) drives the SAME function the painter calls. New qxa case `folder_sum_preview.qxa`: two child tracks each holding `../test_sawtooth.wav` at the same position → the folder's summed envelope is **exactly twice** one child's, column by column, ±1 LSB of the 8-bit quantisation. Both children hold the same file in phase, so this is a closed form, not a measurement. |
| M3.6 | Same case: `set-track-volume` on a CHILD to −60 dB drops that child's contribution (the sum falls to one child's envelope ±1 LSB); `set-track-volume` on the FOLDER changes the folder envelope by **exactly nothing** (byte-identical probe arrays). That pair is the whole semantic rule, gated. |
| M3.7 | Same case: `set-track-mute` on a child drops it out of the sum; muting the FOLDER leaves the sum unchanged. |
| M3.8 | A folder holding only child tracks (no clips of its own) DRAWS the overlay — i.e. `getTrack().isEmpty()` no longer returns before it. Gate: `expectEmpty="false"` on such a folder, plus the PNG in M3.10. |
| M3.9 | A track with NO child tracks draws no overlay (`expectEmpty="true"`), and a folder whose children are empty draws nothing — "at least when they are non-zero". |
| M3.10 | Pixel gate: a canvas PNG over a collapsed folder with material. The overlay pixels on the folder row are (a) present, (b) **strictly lighter** in luminance than that lane's fill colour, and (c) darker than the clip body colour so it reads as background. Asserted from the PNG, not eyeballed. This closes part of the "no verb gates the arranger canvas paint" gap CLAUDE.md records. |
| M3.11 | Nested folders: a two-level fixture where the grandchild's contribution reaches the top folder scaled by the middle folder's gain. One assertion, closed form. |

**A note on the doubling gate's arithmetic, so M3.5 is not written wrong.**
A probe is `peak * 127`, and `test_sawtooth.wav` ramps: its per-second RMS is
0.067 / 0.176 / 0.291 / 0.405, so a sawtooth's peak is about
0.116 / 0.305 / 0.504 / 0.702 and one child's probes are roughly
**15 / 39 / 64 / 89**. Two identical children in phase therefore double to
**30 / 78 / 128 / 178** — and the last two **CLAMP at 127**. So the
exactly-double assertion belongs in seconds 0 and 1; seconds 2 and 3 are where
the gate asserts the CLAMP instead, which is the other half of M3.2 (an `int32`
accumulator that clamps once, rather than a `signed char` that wraps to a
*smaller* value than one child). Both halves are wanted: wrapping and clamping
are the two ways this can be got wrong and they fail in opposite directions.

### M4 — Gates, docs, and the honest PR

| AC | Criterion |
|---|---|
| M4.1 | Full suite green at `-j4`, count reconciled: the M0 registered count PLUS exactly the cases this branch adds, AND a serial run green. Any case failing once is pinned with `repeat_test.sh` from `smaragd/tests/cases/`, swept over `SMARAGD_REVAL_WORKERS` {1,4,8,16}, and reported either way. |
| M4.2 | `check_layering.py` and `check_logging.py` clean. If `main/objects/track` needs a new app edge for `srendercontext.h`, it is added to `tools/check_layering.py` **with a comment saying why**, in the style of the existing entries. |
| M4.3 | Docs updated: `docs/ACTIONS.md` (the new verb), `main/timeline/CONTRACT.md` (a new invariant: the folder overlay is a sum of envelopes, never a freeze, never blocks, and never scales by the lane's own fader), `main/objects/wave/CONTRACT.md` (the rule adopted above), `main/objects/track/CONTRACT.md` (the walk and its audibility rule), `main/testkit/CONTRACT.md` (the new verb), `CLAUDE.md` (a short section), and `plan/STATE.md` (the execution record). |
| M4.4 | **Byte gate**: the committed render goldens are byte-identical to M0.3's baseline. This is a UI-only change; a moved byte is a bug, not a licence. |
| M4.5 | Repaint cost measured and REPORTED, not asserted: time a canvas grab of a collapsed folder over N children, before and after. A bound tight enough to separate the two would be flaky (the `twlog_test` / `log_dock_scale` lesson) — so it is a number in the PR body, not a gate. |
| M4.6 | The PR body says what was gated **and what was not** (below). |

### Not gated, and the PR must say so

- **The sum-of-envelopes approximation itself.** No case asserts that the
  overlay equals the folder's real summed audio, because it does not: children
  out of phase over-state, and child plugins/instruments/automation are invisible
  to it. The in-phase fixture is a closed form precisely because it avoids the
  question.
- **Pixel exactness / colour aesthetics.** M3.10 asserts a luminance relation,
  not a palette.
- **Repaint latency under load.** Measured, not bounded (M4.5).
- **Asset clips' referenced-track fader.** Unchanged by design; see below.
- **Folders deeper than two levels**, and folders holding hundreds of clips.

## Deliberately not done

1. **The exact frozen-page sum** (read each child's `twRewire` pages, sum
   samples, then envelope). Exact, sees plugins and automation, and needs a
   background worker, a published snapshot and an invalidation protocol for
   every child edit — a proposal of its own. The seam this one builds (D1/D2/D3)
   is where it would land, and the overlay's call site would not move.
2. **Stripping the referenced track's fader from an ASSET clip's preview.** It
   would mean building the capture from `cpDspChain_` (pre-gain) rather than
   `cpRewire_`, which changes what `SCut::buildCapture_` means for every
   consumer of the capture, not just the drawn waveform.
3. **Per-channel waveforms.** The channel fold (proposal 36 B8) stands: the
   waveform is one lane, the METER is where per-channel level lives.
4. **Making the overlay configurable.** No option, no per-track toggle. If it
   proves noisy, the answer is the colour, not a preference.

## Execution record

### M0 — baseline, 2026-08-18

Worktree `.claude/worktrees/folder-sum-preview`, branch
`feat/39-folder-sum-preview` off `585f80a`. `./build.sh` green;
`check_layering.py` and `check_logging.py` clean.

`ctest --test-dir smaragd/build -j4`: **200 registered / 197 run / 3 Not Run
(Disabled)** — the disabled three are the macOS-only `au_*` trio — in 221 s,
**196 passed, 1 failed**.

The failure is **`qxa.plugin_strip_nested_track`, and it is PRE-EXISTING**: this
worktree carries no code change at all, so nothing on this branch can have caused
it. Pinned in isolation **5/5 green** from `smaragd/tests/cases/`, which puts it
in the same class as the full-suite-load flakes CLAUDE.md already records
(`clip_properties_actions`, `split_plain_screenshot`). Root cause not
established; not chased here; named in the PR.

Note for the count ACs: CLAUDE.md's "174 registered / 171 run" predates later
cases. **200 / 197 / 3** is the number this branch is measured against.

### M1..M4 — executed 2026-08-18, all green

Six commits on `feat/39-folder-sum-preview`. Final gate: **204 registered / 201
run / 3 Not Run (Disabled)** — **201/201 at `-j4` (194 s) and 201/201 serially
(319 s)**, `check_layering` and `check_logging` clean, `smaragd/tests/goldens/`
**byte-identical** (md5 unchanged either side of both runs). Nothing failed once,
so nothing needed pinning; `qxa.plugin_strip_nested_track` — M0's pre-existing
failure — passed in both runs.

Measured, on the closed-form fixture (`../test_sawtooth.wav`, whole clip in four
columns): one child reads **0 / 25 / 50 / 76**; two children in phase read
**0 / 50 / 100 / 127** (the true sum at column 3 is 152, so it clamps); a child
at −60 dB or muted drops out and leaves one child's envelope exactly; the
FOLDER's own fader and mute leave the array **byte-identical** over 64 columns;
a grandchild under a −6 dB middle folder reads **0 / 13 / 25 / 38**
= `round(p · 10^(−6/20))`. The folder row's pixels: fill `#284664` luma 64,
**7999 overlay pixels at luma 79**, clip body 160, `otherPixels = 0` — and
**identical collapsed and expanded**.

Repaint cost (reported, never asserted — M4.5): 200 canvas grabs at 1200×800
over a folder with six children, median of 3, start-up subtracted —
**6.11 ms/grab with the overlay, 1.78 ms without**, so **+4.3 ms** per
full-canvas repaint. That is the same order the arranger already pays to draw
those clips on their own lanes when the folder is expanded, which is D5's claim.

### Three places this proposal was WRONG, and how each was caught

1. **M2.3's gate could not bite** — see the amended AC. The lesson generalises:
   a gate has to sit on the same side of a seam as the thing it is gating, and
   M1 had deliberately moved the seam *below* the multiply. Caught by executing
   the AC literally — writing the case first and running it before the fix, which
   is the only reason it was noticed rather than shipped as false comfort.
2. **D3's "the SAME visible time range" for every clip was wrong**, and silently
   so. `SCutRendererInline::collectEnvelope` clamps a negative clip-relative
   position to 0 (`cutSourceTimeOf`), so a clip starting *after* the window's
   left edge would have smeared its audio across every column instead of
   occupying the ones it covers. Each clip gets its OWN pixel span, sized the way
   the clip loop sizes the rect it draws that clip into. **Caught by reading, not
   by a gate — and the gate still cannot see it**, because every clip in the
   fixture starts at frame 0. That is the single largest hole left here.
3. **D4's `isEmpty()` claim was wrong.** `SObject::isEmpty()` is
   `childOrder_.isEmpty()` and a folder's child *tracks* ARE child links, so it
   was already false for the common folder and the early-out never swallowed
   anything. Moved below the overlay regardless, because the reading D4 invites
   is the one that would take the overlay away later; the code comment says so.

Two smaller corrections: `SEnvelopeWindow` lives in `app/model/sobjectrenderer.h`
rather than `swaveformdraw.h` (the virtual is declared on `SObjectRenderer`, and
`app/model` may not include `app/objects/wave`), and `envelopeWindowOfContext()`
moved there with it in M3 (the overlay derives the same window for a LANE, and
`objects/track` may not include `objects/wave`). No new module edge was needed
for the feature; M1 added one for a TEST only (`testkit → tw/sources`, so the
unit fixture's `SCut` is sample-backed — the branch every real audio clip takes).

### M3a — the collapsed folder

Added because M3's own report named it as the gap that mattered: the feature is
*sold* on "collapse the folder and you can still see what is under it", and no
verb reached the fold path at all. `collapse-track trackPath= collapsed=`
(ABSOLUTE, not a toggle) drives `SStdMixerView::toggleTrackCollapsed` — the fold
triangle's own call — out through `SMainWindow`. ~57 lines, no new edge.

The gate reads the fold through verbs that already existed rather than a new
probe: `assert-lane-overlay` prints `row=N`, so the empty folder at `trackPath=2`
reads `row=4` open, **`row=2` collapsed**, `row=4` again after expanding;
`assert-lane-alignment` still holds collapsed, which is the rebuild a direct
write of `collapsed_` would have skipped; the folder's own lane still reports its
overlay; and the childSum array is byte-identical to the snapshot taken while it
was open. **This closes a claim, not a suspected bug** — the paint is literally
the same either way, and the case header says so.
