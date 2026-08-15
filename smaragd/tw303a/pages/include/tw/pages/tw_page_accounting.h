#ifndef _TW_PAGE_ACCOUNTING_H_
#define _TW_PAGE_ACCOUNTING_H_

#include <cstddef>
#include <cstdint>

// Process-wide accounting for frozen page memory (proposal 36 B1a).
//
// There is NO twOutputPage pool to instrument. Pages are make_shared on demand
// (twcomponent.cc getOrAllocatePage / freezePage, twtrackmix.cc) into unbounded
// per-component std::map<offset_t, page> caches; CapturePagePool is a DIFFERENT
// type (CapturePageData) and is used in production nowhere. So "how much page
// memory is resident" had no answer at all, and every later phase of proposal 36
// makes a memory claim that needs one — B1b multiplies a page's bytes by its
// channel count and B9.2 has to publish the 8-channel figure.
//
// The instrument is therefore the page's own lifetime, not a pool's free list:
// twOutputPage's constructor adds its sample bytes and its destructor removes
// them again. That is correct no matter WHO owns the page — a component cache, a
// scheduler binding, a stalePredecessor chain, or a shared_ptr an audio callback
// is still holding — which is exactly the property a pool-side counter would not
// have had.
//
// Cost: two relaxed atomic adds per page lifetime, against a 256 KiB allocation
// and a 65536-frame render. It is meant to be left on in every build.
//
// Deliberately NOT counted: sizeof(twOutputPage) itself (~200 bytes of header
// beside 262144 bytes of samples), the std::map nodes, and the std::any internal
// state snapshot, whose size is a component's private business. The number this
// reports is SAMPLE BYTES, which is the number that grows with channel width.

namespace tw {
namespace pages {

struct PageMemoryStats {
    uint64_t pages = 0;   // pages alive right now
    uint64_t bytes = 0;   // sample bytes alive right now
};

class PageAccounting
{
public:
    // Called from twOutputPage's constructor / destructor. Never call directly.
    static void onPageAllocated( size_t sampleBytes );
    static void onPageReleased( size_t sampleBytes );

    // What is resident at this instant.
    static PageMemoryStats global();

    // How many pages this process has ever constructed, and the high-water mark
    // of resident sample bytes. A churn figure and a peak figure: two runs can
    // show the same resident total for very different reasons.
    static uint64_t everAllocated();
    static uint64_t peakBytes();

    // Reset the cumulative counters (NOT the resident ones, which are a fact
    // about live objects and are not ours to invent). For a test that wants to
    // measure one render rather than a whole process.
    static void resetCumulative();

    // --- CapturePagePool reservations ------------------------------------
    //
    // A SECOND, much larger page allocation exists and is easy to miss.
    // CapturePagePool pre-allocates its whole `std::vector<CapturePageData>`
    // in its constructor, and SProject builds one with 2048 pages — about
    // 540 MB, reserved eagerly, per project, whether or not a single capture
    // page is ever handed out.
    //
    // It is instrumented here so a page-memory report cannot be read as "this
    // process holds 12 MB of pages" while half a gigabyte sits beside it.
    // Counted as RESERVATION, not residency: how much of it is in use is the
    // pool's own numAllocatedPages(), and the report asks the project for that.
    static void onPoolReserved( uint64_t pages, uint64_t bytes );
    static void onPoolFreed( uint64_t pages, uint64_t bytes );
    static PageMemoryStats poolReserved();
};

}  // namespace pages
}  // namespace tw

#endif  // _TW_PAGE_ACCOUNTING_H_
