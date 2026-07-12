# tw/pages — CONTRACT

Purpose: the frozen-output page model — twOutputPage (256 KiB / 65536 mono
frames), the PageBase interface, the bounds-safe IOVector view over pages,
and the CapturePagePool used by async revalidation.

Public headers: tw_output_page.h, page_interface.h, io_vector.h,
capture_page_pool.h.

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

How to test: build/bin/io_vector_test.exe (links only tw_pages); page
behavior is exercised by every render qxa case.

Known debt: twOutputPage.samples is always FRAME_CAPACITY (memory over-
allocation for short tails); two aspect enums exist (twRenderAspect here,
twCaptureAspect in tw/schedule) with DIFFERENT bit layouts — do not mix.
