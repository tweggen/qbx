# CLIP_MODEL — how a clip on a track actually works

A "clip" is three cooperating layers. Confusing them is the classic source
of double-audio, silent-clip, and wrong-material bugs.

## The three layers

1. **`SLink` (app/model)** — the PLACEMENT: parent container + `startTime`
   (timeline frames). One `SObject` can be placed many times via many links.
2. **`SCut` (app/objects/cut)** — the WINDOW: `startOffset_` (slip into the
   source), `cutDuration_`, `loopLength_`, grain params. Owns the playback
   chain (reader, optional grain stage, optional loop reader) and, for
   container-backed or grained content, a rendered capture.
3. **`ClipEntry` (tw/mix, inside `twTrackMix`)** — the ENGINE VIEW:
   `{startTime, duration, key, twView*, previousPage}`. The `twView` wraps
   "get me the current component" + the position mapper (MapPosFn).

## Identity: the key is the SLink pointer — never the component

Two cuts of one sample resolve to the SAME shared content component until
their readers exist, and a `twView*` is not what callers hold. So
`insertClip/updateClip/removeClip` match on an opaque `key` — `STrack`
passes the `SLink*`. (History: component-pointer matching either removed
nothing, leaving deleted clips audible/dangling, or removed BOTH cuts of a
sample on a cross-track move.)

## Synchronization STrack → twTrackMix

- `trackChildWasAdded(child)` → `insertClip(&child, startTime, duration,
  getComponentFn, mapPosFn)` on every bus mixer.
- `trackChildWasMoved` (sender IS the SLink — `startTimeChanged` is a link
  signal) → `updateClip(slink, newTime, duration)`.
- `trackChildDurationChanged` — **sender is the OBJECT (SCut), not the
  link** (`durationChanged` is connected on the child's SObject). Resolve
  by scanning `childLinks()` for links referencing the sender and update
  EACH placement. A `dynamic_cast<SLink*>(sender())` here is always null —
  that dead cast once left split heads sounding over their full pre-split
  span (doubled, clipped audio).
- `trackChildWasRemoved(child)` → `removeClip(&child)`.

## Rendering a track page (twTrackMix::freezePage_nolock)

For each clip overlapping `[startPos, startPos+len)`:

```
childPos   = max(0, startPos - clip.startTime)        // clip-relative
childPage  = clip.view->freezePage(childPos, ..., clip.previousPage)
             // twView maps childPos into the component's domain
clip.previousPage = childPage                          // state chain
destOffset = max(0, clip.startTime - startPos)
mixStart   = startPos + destOffset
framesToMix = min(childPage->validFrames,
                  clipEnd > mixStart ? clipEnd - mixStart : 0)   // CLAMP!
mix childPage[0..framesToMix) into page[destOffset..]
```

The clamp is load-bearing: a frozen page always carries a FULL page of
source material (FREEZE_PROTOCOL.md), so without it the last page of every
clip bleeds up to ~1.36 s of extra audio past the clip end.

Track gain/mute applies to the summed page (`trackMuted_` → factor 0).

## The cut's playback chain (SCut::rebuildReader)

- Plain sample cut: `content.getRandomSource()->acquireReader(env, off)` —
  an independent cursor over the shared resident data (never share
  `twWavInput`'s cursor between cuts).
- Grained: interpose `twGrainSource`; offsets scale by `stretch`
  (plainwave domain → stretched domain).
- Looping (`0 < loopLength < cutDuration`): `twLoopReader` with the loop
  base baked in — the reader is CUT-RELATIVE (identity in MapPosFn).
- Container-backed (content is a track/mixer): render the content once via
  freezePage into an owned `twCapturingSource`, read that like a sample.
- Rebuilds are chain-descriptor-checked: a plain trim/slip reuses the
  existing reader (slip is applied per-position via MapPosFn, and source-
  keyed page caches stay valid — POSITION_DOMAINS.md rule 4).

## Duration semantics

`SCut::getDuration()` (snapshot) is the TIMELINE duration of the window,
already including stretch. `SPlainWave::getDuration()` is the source length
in project-rate frames (`twWavInput::getLength()` reports via
`viewAtRate(projectRate)`; a truncated file is clamped to real data at load
with a warning — the header's claim is not trusted).

## Take stacks (proposal 17): a fourth, optional layer

A multi-take clip inserts `STakeStack` between the placement and the
window: `SLink → STakeStack → (SLink → SCut)*`. The stack is the COLUMN of
parallel takes; exactly one is audible (`activeTake_`, -1 = none). To
`twTrackMix` a stack is ONE clip keyed by the OUTER link: the stack
delegates `getRootComponent`/`mapTimelineToComponentPos`/preview to the
active take's cut, and the clip's `twView` resolves that lazily — so
`select-take` is just a model change + `durationChanged` (→ `updateClip`,
which bumps the epoch AND resets the clip's state-chain page, because the
component identity changed) + `invalidateRenderPath`.

Rules:
- All take cuts share the stack's timeline duration. Length edits go
  through `setDurationAll`/`applyWindowAll` (slip offsets rescale on
  stretch change); slip/pitch stay per-take.
- Take links' startTime is always 0; the OUTER link owns placement.
- A stack exists only with ≥2 takes: `stakes::wrapCutLinkIntoStack` /
  `collapseSingleTakeStack` convert in place, PRESERVING the child index
  (recorded action/inverse paths must stay valid).
- Splitting a stack splits every take with the plain-cut arithmetic
  (offsets/durations live in the stretched output domain, so the timeline
  split offset applies per take verbatim).
