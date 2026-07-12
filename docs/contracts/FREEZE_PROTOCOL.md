# FREEZE_PROTOCOL — random-access page rendering

`twComponent::freezePage()` is the random-access side of the component
contract: render THIS component's output for an explicit window into a cached
`twOutputPage`. It is how offline render, container captures, and previews
work without fighting the single cursor of the live streaming path.

## The normative sequence (twComponent::freezePage_nolock)

```
page->startPosition is AUTHORITATIVE — never trust the live cursor.

contiguous  := previousPage && previousPage->validAspects != 0
            && previousPage->startPosition + previousPage->validFrames
               == startPos

if contiguous:  restoreInternalState(previousPage->internalState)
else:           reset()                    // state can't be reconstructed
seekTo(startPos)                           // ALWAYS — position is generic,
seekInputStreams(startPos)                 // state is not
renderFrames(page->samples, FRAME_CAPACITY, ...)
page->internalState = captureInternalState()
```

Rationale: every seekable component can jump to a position, but a reverb
tail or filter memory cannot be reconstructed for an arbitrary position.
So POSITION is set explicitly in both cases (`seekTo` must therefore be
state-preserving — it is a position operation, not a reset), while STATE
only carries across a contiguous page chain.

## Caching and concurrency

- Pages live in the component's `outputPages_` map keyed by `startPos`
  (in the component's OWN domain — see POSITION_DOMAINS.md rule 4).
- Cache check + placeholder insertion happen under the component mutex;
  the RENDER happens outside it (upstream freezePage calls recurse).
  `validAspects == 0` marks a placeholder still being rendered.
- `FreezeContext` (thread-local) detects freeze cycles: if a component's
  own render path re-enters freezePage on itself, it gets silence instead
  of infinite recursion.
- `page->pageMutex` protects `internalState` reads/writes across
  multi-consumer access; `generation` increments on invalidation so audio
  threads can detect stale pages lock-free.
- `invalidateAllPages()` zeroes `validAspects` on every page and cascades
  to registered dependents.

## Page geometry

- `twOutputPage::PAGE_SIZE` = 256 KiB, `FRAME_CAPACITY` = 65536 mono
  frames (≈1.365 s @ 48 kHz). Pages are FULL units: callers should request
  page-aligned positions and extract sub-ranges (RenderSession does).
- A page always carries a full page of the component's material; consumers
  that represent a bounded window (a clip!) must clamp what they mix out of
  it (see CLIP_MODEL.md — the clip-end-bleed bug).

## Sequential consumers

RenderSession is the canonical sequential consumer: it freezes pages
page-aligned from the range start, passes each page as `previousPage` of the
next (so DSP state chains), and extracts `[currentPos % PAGE_SIZE ...]` per
block. First page of a run has `previousPage == nullptr` → discontinuity →
reset, which is correct.

`twTrackMix` chains per-clip: `ClipEntry::previousPage` holds each clip's
last frozen page so clip-internal state carries across track pages.

## Preview variant

`freezePreviewPage(startPos, length, previewRate, fullRate, prev)` renders
at a reduced rate (typically 1 kHz) for waveform display through the same
protocol; the CaptureRevalidator drives it off the UI thread.
