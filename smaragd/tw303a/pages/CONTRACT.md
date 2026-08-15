# tw/pages — CONTRACT

Purpose: the frozen-output page model — twOutputPage (256 KiB / 65536 frames
PER CHANNEL, planar, one channel wide everywhere in the tree today), the
PageBase interface, the bounds-safe IOVector view over pages, and the
CapturePagePool used by async revalidation.

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
   `getDataPtr()` is channel 0, so a width-1 call site stays correct. There is
   no raw `samples.data()` anywhere outside this class — that is what makes
   "every consumer was converted" a fact the compiler checks. An out-of-range
   channel is reported once and answered with channel 0, so it can never be an
   out-of-bounds read on the audio thread; the §4.4 clamp
   (min(latchIndex, page->channels - 1)) is a PLUG rule and belongs in
   twStreamingLatch, not here.
9. `resizeMonoScratch()` is the pre-existing throwaway-buffer path
   (twComponent::calcOutputTo, IOVector::CreateFromBuffer) and is legal at
   width 1 only; on a wider page it is refused and logged, never applied.

How to test: `ctest -R page_channels` (the channel dimension: stride
arithmetic, a four-channel round trip through channelPtr with signals whose
value ranges are pairwise disjoint so a single sample names its channel, and
the type-level immutability of `channels`), `ctest -R io_vector` (pages/tests/,
links only tw_pages) and `ctest -R graph_test` (the accounting arithmetic, from
tw_graph, which is where the per-component half lives); page behavior is
exercised by every render qxa case, and `<report-page-memory>` prints the
figures from inside one.

Known debt: a page's storage is always channels * FRAME_CAPACITY (memory over-
allocation for short tails); two aspect enums exist (twRenderAspect here,
twCaptureAspect in tw/schedule) with DIFFERENT bit layouts — do not mix.
There is NO twOutputPage pool: pages are make_shared on demand into unbounded
per-component maps in tw/graph, and the accounting added in B1a measures that
fact rather than changing it. CapturePagePool is a SEPARATE thing serving a
separate page type (CapturePageData, the preview/metadata capture page), and it
is not a small one: it pre-allocates its whole std::vector in its constructor and
SProject asks for 2048 pages — 553 648 128 bytes reserved eagerly per project,
of which a 4 s corpus render uses ONE. It is accounted separately
(PageAccounting::poolReserved) precisely so nobody reads a twOutputPage figure as
this process's page memory. Proposal 36 B1 calls it "used in production
nowhere"; that is wrong on both halves.

IOVector is deliberately NOT channel-aware (proposal 36 §4.6): it is a mono view
and reads channel 0 of whatever pages it holds. Wide mixing is a loop over
channels of the same page pair, so the channel belongs to the loop, not to the
view — but §4.6's own phrasing, "a view over one channelPtr(c)", implies a
channel selector that B4 will have to add when it finally has a caller.
