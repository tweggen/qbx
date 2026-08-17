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

- `twOutputPage::PAGE_SIZE` = 256 KiB, `FRAME_CAPACITY` = 65536 frames
  PER CHANNEL (≈1.365 s @ 48 kHz). Pages are FULL units: callers should request
  page-aligned positions and extract sub-ranges (RenderSession does).
- A page is PLANAR and carries its own channel count (proposal 36 §4.1): channel
  *c* occupies `[c * CHANNEL_STRIDE, …)` of the buffer, `CHANNEL_STRIDE` is the
  constant `FRAME_CAPACITY`, and `channels` is IMMUTABLE after allocation. This
  paragraph said "65536 **mono** frames" until proposal 36 B4, which is the
  milestone where a production component first freezes a wider one: the track
  path (`twTrackMix` → `twPluginChain` → `twPluginInsert` → `twRewire`) and the
  master (`twMixer` → `twRewire`) are all `SProject::channels()` wide. A
  component's width is `getOutputChannels()`; `freezePage_nolock` forks on the
  width of the PAGE IN HAND, and a page whose width disagrees with its
  producer's declared width is a MISS, never audio (§4.5). The remaining
  contract sweep is proposal 36 B9.
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

`SObject::straightCalcPreviewData()` is the third one: a CONTAINER's waveform
peaks are scanned out of the same chained page sequence (the app model may not
seek a live component — app/model/CONTRACT.md inv. 9). A page the component
cannot produce reads as silence; the preview never waits and never declares a
demand.

## Class-1 consumers: an instrument is not "reset and carry on"

Proposal 19's execution-class analysis calls a component CLASS 1 when its DSP
state at a position depends on material that is not in the page being rendered.
An effect is class 1 in a mild way — a reverb tail, a filter's poles — and its
answer to a discontinuity has always been the one above: `reset()`, then render
what the page's own input gives.

An INSTRUMENT (proposal 37 P3b: `twPluginSlotProcessor` in a generator mode) is
class 1 in a way that reset alone cannot serve, because the note sounding at P
had its note-on pages ago and there is no upstream page to read it from — the
note lives in MODEL data (a `twEventSource`), not in the dataflow. A page whose
`startPos` is not the processor's `lastEnd_` therefore runs the full D4
protocol instead:

    reset()                       all notes off
    chase stateAt(P - K)          held notes + every controller that shaped them,
                                  as events at offset 0
    pre-roll K frames             the real events at their real offsets, OUTPUT
                                  DISCARDED
    render the page               without ever re-issuing the page's own chase

    K = min( max(4096, tailFrames(), P - start(earliest note held at P)), 4 s )

Three things follow, and they matter to anyone touching the scheduler:

- **No new plan.** An instrument needs no upstream page for its pre-roll, so it
  needs no `planPage` override (plugins inv. 14 stands). The pre-roll is
  entirely inside one `freezePage`.
- **Every instrument page is a pure function** of its position and the feed.
  Out-of-order freezing, a re-render after an invalidation and a cold first
  page all produce the same bytes — which is what lets an event edit be gated
  by a byte compare of the region before it (`instrument_edit_reaches_render`).
- **An epoch bump does not clear `lastEnd_`.** Only a rebuild, a rate change
  and `forgetContinuity()` do. The RUN BARRIER of design D4 (built in P3c) is
  what calls the last of those: without it a render whose first page starts
  exactly where a previous run stopped would CONTINUE that run's voices.

The cost is real: `K` grows with P for a long held note, and a page is a
reposition whenever it is not contiguous. It is bounded at four seconds and is
recorded as known debt in `tw303a/plugins/CONTRACT.md`.

## The run barrier (proposal 37 P3c, design D4 / 4.4)

A RUN is one contiguous traversal of the graph by a consumer: an offline
render, or a playback start. Runs are where class-1 state leaks between
consumers, so every run opens with a barrier — and the barrier is an APP-SIDE,
MAIN-THREAD act, not anything the freeze protocol or the scheduler does:

    SApplication::beginRun(pos)
        for every track whose slot 0 is an INSTRUMENT:
            slot->forgetContinuity()                       // clears lastEnd_
            track->invalidateRenderPathRange(pos, INT64_MAX)

Five properties, each of which is a decision and not an accident:

- **Full path, from the app.** `invalidatePagesInRange_nolock` does not
  cascade: it bumps one component and re-blesses that component's own pages
  (design F13). The consumers decide "current" against the ROOT's epoch, so the
  only thing that carries a tap's change up to them is the `SObject`
  invalidation walk on the main thread. A barrier issued anywhere in the engine
  would reach nothing anyone looks at.
- **Both halves.** An epoch bump does not clear `lastEnd_`, and clearing
  `lastEnd_` does not stale a cached page. Either alone is a hole.
- **In that order** — `forgetContinuity()` first, then the bump. A page
  rendered after the bump is then guaranteed to have seen the cleared
  continuity; one rendered in between is staled by the bump and re-rendered.
- **Before the first demand, never inside a render.** `startRender()` issues it
  before the session thread spawns; the play-start paths issue it immediately
  before `twSpeaker::startOutput()`, which performs the engine's pre-readahead
  `seekTo(locator)` + `startReadahead()` on the same thread. An epoch bump of a
  component from inside its own render livelocks the scheduler (F10, schedule
  inv. 8).
- **Open-ended on the right.** `[pos, INT64_MAX)`: a class-1 consumer's change
  at `a` can be heard at any later position, so a bounded range would re-bless
  a continuation page rendered from pre-barrier state (F9).

Where it is deliberately NOT issued, and what that costs:

- **A locate while stopped** demands nothing — `requestSeek` only runs while
  playing — so the barrier at the next play start covers it. That is why
  `setGlobalLocatorPos` does not call it.
- **A seek during playback, and a loop wrap.** Those keep today's
  page-boundary splices. The RT thread adopts a fresh current-epoch page MID
  PAGE as soon as it lands (F14, proposal 16), so re-staling what it is serving
  would be an audible switch at an arbitrary offset — worse than the splice.
  Chase + pre-roll already make the hole page approximately right. ACCEPTED and
  NOT GATED; every PR body says so.
- **Effects are not barriered at all.** Their splice at a page boundary is what
  they have always done.

Idempotent under any ordering: a late barrier costs one re-render, never a
wrong page served as current (verify-at-publish self-staleness). Measured cost
on the P3c gate project (one instrument track, playback starting on a page
boundary the previous render had already frozen): TWO pages re-rendered per
play start, ~20 ms — see STATE.md 2026-08-16.

## Preview variant

`freezePreviewPage(startPos, length, previewRate, fullRate, prev)` renders
at a reduced rate (typically 1 kHz) for waveform display through the same
protocol; the CaptureRevalidator drives it off the UI thread.
