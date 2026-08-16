# tw/pages — CONTRACT

Purpose: the frozen-output page model — twOutputPage (256 KiB / 65536 mono
frames), the PageBase interface, the bounds-safe IOVector view over pages,
and the CapturePagePool used by async revalidation.

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
5. Page memory is accounted at the PAGE's own lifetime, not a pool's.
   twOutputPage's constructor calls
   tw::pages::PageAccounting::onPageAllocated with the sample bytes it just
   reserved and remembers that number in accountedBytes_; the destructor
   returns exactly that number. A page is therefore built at its final size —
   the counters are exact only because nothing resizes a page in place.
   Consequence worth knowing: the counter sees pages NO POOL WOULD — one bound
   into a scheduler node, held by an audio callback, or hanging off a
   stalePredecessor chain is resident memory and is counted.

How to test: `ctest -R io_vector` (pages/tests/, links only tw_pages) and
`ctest -R graph_test` (the accounting arithmetic, from tw_graph, which is where
the per-component half lives); page behavior is exercised by every render qxa
case, and `<report-page-memory>` prints the figures from inside one.

Known debt: twOutputPage.samples is always FRAME_CAPACITY (memory over-
allocation for short tails); two aspect enums exist (twRenderAspect here,
twCaptureAspect in tw/schedule) with DIFFERENT bit layouts — do not mix.
There is NO twOutputPage pool: pages are make_shared on demand into unbounded
per-component maps in tw/graph, and the accounting measures that fact rather
than changing it. CapturePagePool is a SEPARATE thing serving a separate page
type (CapturePageData, the preview/metadata capture page), and it is not a
small one: it pre-allocates its whole std::vector in its constructor and
SProject asks for 2048 pages — 553 648 128 bytes reserved eagerly per project,
of which a short render uses ONE. It is accounted separately
(PageAccounting::poolReserved) precisely so nobody reads a twOutputPage figure
as this process's page memory.
