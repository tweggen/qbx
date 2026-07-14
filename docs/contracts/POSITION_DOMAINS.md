# POSITION_DOMAINS — who speaks which time domain

Every position bug in this codebase (including the 2026-07-12
split-clip-renders-from-file-start bug) has been a confusion between these
domains. Learn the table, then the rules.

## The domains

| Domain | Unit | Zero point | Who speaks it |
|---|---|---|---|
| **Timeline (project)** | frames @ project rate | project start | transport, locator, `SLink::startTime`, `twTrackMix::playOffset_`, RenderSession ranges |
| **Clip-relative** | frames @ project rate | clip's `startTime` | what `twTrackMix` hands a clip: `pos - clip.startTime` |
| **Component/source** | frames @ project rate | source material start (or stretched/loop-window equivalents) | `twSampleReader::pos_`, `twWavInput::playOffset_`, page-cache keys |
| **Native file** | frames @ file rate | file start | `twSampleSource` internals only |

## The rules

1. **Tracks speak clip-relative.** `twTrackMix::seekTo_nolock` and
   `freezePage_nolock` compute `pos - clip.startTime` and hand it to the
   clip's `twView`. They know nothing about slip offsets.

2. **`twView::MapPosFn` is the only translator.** The view maps
   clip-relative → component domain before `seekTo`, `freezePage`, and
   `freezePreviewPage`. `STrack` supplies the mapper per clip:
   `child.getSObject().mapTimelineToComponentPos(off)`.

3. **`SObject::mapTimelineToComponentPos` defaults to identity.**
   `SCut` overrides it (mirrors `SCut::seekTo`):
   - looping reader (`twLoopReader`): identity — the loop reader is
     cut-relative, it owns its loop base internally;
   - plain reader (grained or not): `off + startOffset` — the cut window
     (`startOffset`, `cutDuration`, `loopLength`) is stored in the grain
     OUTPUT (stretched) domain, the same domain the grain view is addressed
     in, so no stretch conversion is applied (the source position is
     `startOffset / stretch`).
   The override calls `ensureReader()` first, so the component the track
   talks to is the cut's OWN reader, never the shared `twWavInput` cursor.

4. **Page caches are keyed in the component's OWN domain.** A frozen page on
   a sample reader is keyed by SOURCE position. Consequence: slipping a clip
   (changing `startOffset`) does NOT invalidate its pages — a slipped clip
   simply requests different source pages. Changing stretch/loop mints a new
   reader (fresh cache) via `SCut::rebuildReader`'s chain-descriptor check.

5. **Rate conversion happens at exactly one seam.**
   `twWavInput::getSource()` and `calcOutputTo` go through
   `twSampleSource::viewAtRate(env.getSRate())`. Everything above that seam
   (readers, cuts, tracks, mixer, render) speaks project-rate frames. Never
   convert rates anywhere else; never hand out the native-rate source.

6. **RenderSession positions are absolute timeline.** The render loop's page
   positions are `startOffsetSamples_ + samplesWritten` (a marked range does
   NOT start at 0). The app's locator is also absolute timeline; sessions
   publish it via `onPosition` callbacks.

## Historical failures these rules encode

- Tail of a split clip played the source from 0: the freeze path seeked the
  reader with a clip-relative position and nobody added `startOffset`
  (fixed by rules 2–3).
- Marked range starting at t>0 rendered the region starting at 0: rule 6.
- Clip audio bleeding ~0.19 s past its end: a frozen page always carries a
  full page of source; the TRACK must clamp mixing to the clip window (see
  CLIP_MODEL.md).
