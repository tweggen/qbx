# tw/pages — CONTRACT

Purpose: the frozen-output page model — twOutputPage (256 KiB per channel /
65536 frames per channel, planar, **N channels wide: the project's width, 1/2/
4/6/8**), the PageBase interface, the bounds-safe IOVector view over pages, and
the CapturePagePool used by async revalidation.

*(This line said "one channel wide everywhere in the tree today" until proposal
36 B9. It was written at B1b, when it was true and load-bearing — the whole
safety argument of that milestone was that the mechanical sweep happened while
every page was still one channel wide. B4 widened the track path and B5 the
sink; the sentence outlived its milestone by five of them.)*

Public headers: tw_output_page.h, page_interface.h, io_vector.h,
capture_page_pool.h, tw_page_accounting.h.

Depends on: tw/core. Forbidden: tw/graph and above (pages carry component
OUTPUT; they must not know components).

Threading: page->pageMutex protects internalState and metadata updates;
validAspects and generation are atomics readable lock-free from audio
threads; the pool is internally synchronized.

Invariants:
1. page->startPosition is authoritative for content (FREEZE_PROTOCOL.md);
   validAspects == 0 marks an unrendered placeholder.
2. Pages are FULL FRAME_CAPACITY units; consumers extract sub-ranges and
   bounded consumers must clamp (CLIP_MODEL.md).
3. IOVector operations are bounds-safe by construction — never hand out raw
   pointers across page boundaries (CreateFromBuffer is legacy interop only).
4. generation increments on invalidation so lock-free readers detect staleness.
5. Page memory is accounted at the PAGE's own lifetime, not a pool's
   (proposal 36 B1a). twOutputPage's constructor calls
   tw::pages::PageAccounting::onPageAllocated with the sample bytes it just
   reserved and remembers that number in accountedBytes_; the destructor
   returns exactly that number. A page that changes width must therefore be
   built at its final width — the counters are exact only because nothing
   resizes a page in place, and B1b's channel dimension must keep it that way.
   Consequence worth knowing: the counter sees pages NO POOL WOULD — one bound
   into a scheduler node, held by an audio callback, or hanging off a
   stalePredecessor chain is resident memory and is counted.
6. A page's CHANNEL COUNT IS IMMUTABLE (proposal 36 §4.5, built by B1b) and is
   enforced by the type: `channels_` is a const member, the sample buffer is
   private, there is no setter, and the page is neither copy- nor
   move-assignable. Later phases treat a stale page whose width differs from
   what a consumer expects as a MISS rather than as audio, and that rule is
   only sound if a width cannot change under a reader.
7. The layout is PLANAR with a CONSTANT stride: channel c lives at
   `c * CHANNEL_STRIDE`, and CHANNEL_STRIDE is FRAME_CAPACITY — never
   validFrames, which shrinks as a tail runs out and would make the stride a
   read-time question. Storage is `channels * CHANNEL_STRIDE`.
8. Sample data is reached ONLY through `channelPtr(c)`; `channelFrames()` (per
   channel), not `sampleCount()`, is what a frame index is bounded by.
   `getDataPtr()` — which returned channel 0 so a width-1 call site stayed
   correct — was **deleted at B9**: it had no callers, and a width-blind
   pointer into a planar buffer is the hole this invariant exists to close.
   There is no raw `samples.data()` anywhere outside this class — that is what makes
   "every consumer was converted" a fact the compiler checks. An out-of-range
   channel is reported once and answered with channel 0, so it can never be an
   out-of-bounds read on the audio thread; the §4.4 clamp
   (min(latchIndex, page->channels - 1)) is a PLUG rule and belongs in
   twStreamingLatch, not here.
9. `resizeMonoScratch()` is the pre-existing throwaway-buffer path
   (twComponent::calcOutputTo, IOVector::CreateFromBuffer) and is legal at
   width 1 only; on a wider page it is refused and logged, never applied.
10. WIDTH MISMATCH IS A MISS (proposal 36 §4.5, wired by B2).
    `twPageWidthUsable(page, producerChannels)` is the one place the rule
    lives: a cached page whose width disagrees with what its producer now
    declares is treated as a MISS — playback falls back to silence for that
    page, a meter decays — and never as audio. It matters because the
    stale-page fallback (proposal 16) deliberately serves stale pages during
    live playback, so that is the ONE path on which a page frozen before a
    width change can reach the RT callback or twLevelProbe, where reading
    channelPtr(1) of a width-1 page would be an out-of-bounds read on the audio
    thread. Every acceptance in AudioEngine::updateFrozenPage and every rung of
    twLevelProbe's ladder is gated on it. A fresh page always passes (pages are
    allocated at their producer's declared width), and a correctly-WIDE page is
    never rejected merely because its consumer only reads channel 0 — that
    consumer is narrow, not wrong. Gated by metering_test; the RT half is
    AC B4.5's to prove. (That clause read "since nothing in production is
    wide before B4" when it was written; production has been wide since.)

How to test: `ctest -R page_channels` (the channel dimension: stride
arithmetic, a four-channel round trip through channelPtr with signals whose
value ranges are pairwise disjoint so a single sample names its channel, and
the type-level immutability of `channels`), `ctest -R wide_graph_test` (the
same page through the REAL scheduler, plus the §4.4 plug/bound-page rules and
the width > 1 refusal), `ctest -R metering_test` (inv. 10), `ctest -R io_vector` (pages/tests/,
links only tw_pages) and `ctest -R graph_test` (the accounting arithmetic, from
tw_graph, which is where the per-component half lives); page behavior is
exercised by every render qxa case, and `<report-page-memory>` prints the
figures from inside one.

Known debt: a page's storage is always channels * FRAME_CAPACITY (memory over-
allocation for short tails); two aspect enums exist (twRenderAspect here,
twCaptureAspect in tw/schedule) with DIFFERENT bit layouts — do not mix.
There is NO twOutputPage pool: pages are make_shared on demand into
per-component maps in tw/graph. Those maps were UNBOUNDED until proposal 36 B9
— B1a's accounting measured the fact rather than changing it, and B9 measured
what it cost (one 60-second render left 681 pages resident and freed none:
357 MB at width 2, 1.42 GB at width 8) and then wired the pruning that had
existed unused since the beginning. An offline render now drops its trail as it
advances and holds ~108 pages whatever its duration. PLAYBACK STILL DOES NOT
PRUNE — see tw/graph inv. 8 for exactly what is and is not bounded. CapturePagePool is a SEPARATE thing serving a
separate page type (CapturePageData, the preview/metadata capture page), and it
is not a small one: it pre-allocates its whole std::vector in its constructor and
SProject asks for 2048 pages — 553 648 128 bytes reserved eagerly per project,
of which a 4 s corpus render uses ONE. It is accounted separately
(PageAccounting::poolReserved) precisely so nobody reads a twOutputPage figure as
this process's page memory. Proposal 36 B1 calls it "used in production
nowhere"; that is wrong on both halves.

CAPTUREPAGEDATA DOES NOT CARRY CHANNELS, and that is a decision, not an
omission (proposal 36 §4.1, decided and measured at B7 — AC B7.4). Widening it
naively would have made SProject's eager pool 4.2 GiB at width 8. It stays one
plane wide because IT IS NOT AUDIO STORAGE: its 256 KiB payload is an ASPECT
page — FLOAT SAMPLES of the object's output decimated to ~1 kHz (channel 0,
from position 0) written by CaptureRevalidator::dispatchRecomputation, or a
Metadata/Export blob — reached through its plain `data` member (the
`getDataPtr()` accessor went at B9 with the rest of the width-blind API), with
no notion of a frame, a stride or a channel anywhere in its type. Nothing on the
audio path allocates one.

SAY "FLOAT SAMPLES", NOT "PREVIEW WAVEFORM": the loose wording is what proposal
36 trap 26 was. The waveform the user sees is a preview_t {int8 min, int8 max}
PROBE ARRAY, a different thing entirely, computed by
SObject::straightCalcPreviewData / SCut::ensureCapturePeaks and persisted in the
"preview.peaks" sidecar — it never travels through a CapturePageData.
SPlainWave::getPreview nonetheless reinterpret_cast this buffer to preview_t*
until B8 removed it; that branch was unreachable (only SCut ever schedules a
revalidation, so a non-SCut object's currentPage_ is always null), which is why
it never drew anything wrong. A Preview aspect page's payload has NO reader in
the tree: its only consumer, SCut::getPreview, uses the page's existence as a
readiness signal. Before giving it one, give it a geometry. Measured: 2048 pages ×
270 336 B = 553 648 128 B reserved per project, against a peak occupancy of ONE
page across the whole 4 s corpus and 9 in the busiest capture-heavy case. The
name is the trap — CapturePagePool and SCut::buildCapture_ share a word and
share nothing else.

A capture-backed clip's AUDIO width is carried by twCapturingSource instead: a
planar channels * nFrames float buffer, allocated on demand, per clip, sized to
the material rather than to a pool. That is the one allocation in the engine
that multiplies by channel width, so B7 gave it an instrument of its own
(PageAccounting::onCaptureAllocated / capturesResident(), reported as
`captureBuffers=` beside the other two figures). Measured on the corpus: the
asset clip's capture is 115 200 B at width 1 and 230 400 B at width 2 — exactly
×N, and nothing else moved.

Whether the POOL should shrink or become lazy is a real question and B7
deliberately did not answer it: it is not a WIDTH question — 528 MiB for one
page in use is as wrong at width 1 as at width 8 — and changing an eager
reservation that every `-j` headroom figure in CLAUDE.md is written against is
not something to slip into a channels milestone. Recorded for B9.

IOVector is deliberately NOT WIDTH-aware (proposal 36 §4.6): it is a view over
ONE CHANNEL of whatever pages it holds, named by a `channel` constructor
parameter (default 0). Wide mixing is a loop over channels of the same page
pair, so the width belongs to the loop, not to the view. B1b named the channel
once as a constant so that this would be a parameter rather than another sweep;
B4 is the milestone that added it, with twTrackMix's per-channel clip mix as its
first caller. The §4.4 clamp for a NARROWER source (twPageClampChannel) is the
caller's decision — a view may not implement a downmix policy.
