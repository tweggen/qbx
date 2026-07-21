# Proposal 23: clips that start before their data

**Status:** ✅ EXECUTED (option A — signed positions) — 2026-07-21

## Why

A clip may be longer than the sample behind it — the right-edge extend and the
left-edge trim both allow it, and "Remove loop" deliberately produces one. The
remaining asymmetry was the FRONT: the left-edge drag was pinned at the data
start, so a clip could not begin before its material. The same pin blocked
"slip further than the data" and dragging a looped clip by its start boundary.

## Root cause (proven, not inferred)

    tw303a/core/include/tw/core/twtypes.h:13
        typedef unsigned long long offset_t;      // <-- the actual root

`offset_t` — the engine's position type — was itself UNSIGNED. A clip anchored
before its data resolves to a negative position, which wrapped to ~1.8e19 at the
very first cast (inside `SCut::resolveClip`), so the page was requested at an
absurd offset and came back silent. The `(uint64_t)` casts in `twview.cc:84` and
`twtrackmix.cc:373` were no-ops on an already-unsigned type — an early diagnosis
blamed them, but they sat downstream of the real defect.

Measured on a clip anchored 2 s ahead of its sample (page = 65536 frames):

| page | timeline range | maps to | result          |
|------|----------------|---------|-----------------|
| 1    | 65536..131072  | -30464  | wraps -> SILENT |
| 2    | 131072..196608 | +35072  | correct         |

Observed: silence to 2.73 s instead of 2.0 s, then entry mid-ramp at the level
for source 0.73 s — the first ~0.7 s of the sample dropped. `SCut::resolveClip`
computes correctly (probed: `off=0 -> mappedPos=-96000`); the loss was entirely
at the unsigned store. Probes confirmed NONE of `twSampleSource::read`,
`twGrainSource::read` or `twSampleReader::seekTo` was ever reached with a
negative value — the wrap happened above them.

## Options considered

- **A. Signed positions end to end** (CHOSEN). Fixes the defect where it is;
  nothing else has to model silence. Cost: widest blast radius (the core of the
  proposal 19 dataflow), floor-division hazards on every page index, and page
  maps that must tolerate negative keys.
- **B. A leading-silence stage in the clip's reader chain.** Insert a
  `twRandomSource` stage that prepends N frames of silence so the clip is
  addressed from 0 and positions stay unsigned. Smaller blast radius, but it
  expresses "started early" as a fake source, and `clipToReaderMap` /
  `clipToSourceMap` / preview / invalidation all have to fold N consistently.
- **C. Keep the pin.** Zero risk, leaves the asymmetry.

## What landed

1. `offset_t` is now `signed long long`, with the reason recorded at the typedef.
2. `twFloorAlign()` added to `twtypes.h`. Page alignment used
   `(pos/grain)*grain`; C++ truncates toward zero, so -30464 aligned to 0
   instead of -65536 and the page holding the silence-to-data seam was never the
   page rendered. Applied at the streaming-latch seam and both audio-engine
   alignment sites.
3. `twEditRange` made signed, and its unbounded sentinel moved `UINT64_MAX` ->
   `INT64_MAX`. Left as unsigned it would have become -1 — comparing BELOW every
   real position, silently collapsing an unbounded range to `empty()`.
4. Position-carrying `uint64_t` converted to `offset_t` across graph / mix /
   pages (`startPos`, `pageStart`, `startPosition`, page-map keys,
   `invalidatePagesInRange`, `twPageDep`, `twPagePlan`). `inputOffset` stayed
   unsigned deliberately: it indexes a buffer, not a timeline.
5. `twTrackMix` plan and mix arithmetic made signed end to end, so `mappedPos`
   reaches `twPageDep` intact.
6. **Prerequisite:** `twSampleSource::read` and `twGrainSource::read` were both
   out-of-bounds on a negative offset — `avail = nFrames_ - srcOffset` GROWS
   when `srcOffset < 0`, so nothing clamped and
   `data_.data() + ch*nFrames_ + srcOffset` pointed before the buffer, then
   `memcpy`. Both now emit leading silence and read the remainder from frame 0.
   Latent only because of the wrap: fixing the type without these would have
   turned silence into a heap OOB read.
7. The UI pin in `SMVActualView::mouseMoveEvent` (left-edge branch) is gone; only
   the TIMELINE start is still pinned at 0, since there is no time before zero.

## Verification

Before / after on a clip anchored 2 s ahead of its data, per-second RMS of the
render (fixture levels sec0 .067 / sec1 .176 / sec2 .291 / sec3 .405):

| region | before          | after      |
|--------|-----------------|------------|
| 0-2 s  | silent to 2.7 s | silent ✓   |
| 2-3 s  | .052 (late)     | .0667 ✓    |
| 3-4 s  | .1764           | .1764 ✓    |
| 4-5 s  | .2906           | .2906 ✓    |
| 5-6 s  | .4055           | .4055 ✓    |

Content now begins exactly at 2.0 s and ramps from .0067 — the sawtooth's true
first frame — with nothing dropped at the seam.

New case `clip_starts_before_data` drives the START edge past the data through
the REAL mouse handlers (`drag-clip-edge`) and asserts leading silence plus each
second at its own level. Its `minRms="0.055"` floor on the first content second
is chosen to exclude the buggy .052, so the case discriminates rather than
merely passing.

## Follow-ups

- Slip-past-data and start-boundary loop drags are now unblocked in the engine;
  the gestures themselves are still to be written.
- ~100 `uint64_t` position sites remain in `audio_engine` playback, where the
  playhead is genuinely >= 0. They are consistent today but should migrate to
  `offset_t` so the type tells the truth everywhere.
- Page maps now admit negative keys. `releaseOldPages` and the request/ready
  dedup keys were reviewed and are order-based (`std::map` ordering holds for
  negatives), but any future code assuming "position 0 is the lowest page" is
  now wrong.
