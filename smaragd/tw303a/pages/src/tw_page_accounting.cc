#include "tw/pages/tw_page_accounting.h"

#include <atomic>

namespace tw {
namespace pages {

namespace {

// Function-local statics rather than namespace-scope ones: a twOutputPage can
// outlive static destruction order in principle (a detached worker's last
// shared_ptr), and a Meyers singleton of trivially-destructible atomics is
// constant-initialized, so there is no window in which the counters do not
// exist. std::memory_order_relaxed throughout — these are statistics, not a
// synchronization mechanism, and nothing may be inferred from their ordering
// with respect to the page contents.
std::atomic<uint64_t> &livePages()
{
    static std::atomic<uint64_t> v{ 0 };
    return v;
}

std::atomic<uint64_t> &liveBytes()
{
    static std::atomic<uint64_t> v{ 0 };
    return v;
}

std::atomic<uint64_t> &everPages()
{
    static std::atomic<uint64_t> v{ 0 };
    return v;
}

std::atomic<uint64_t> &peakLiveBytes()
{
    static std::atomic<uint64_t> v{ 0 };
    return v;
}

std::atomic<uint64_t> &poolPages()
{
    static std::atomic<uint64_t> v{ 0 };
    return v;
}

std::atomic<uint64_t> &poolBytes()
{
    static std::atomic<uint64_t> v{ 0 };
    return v;
}

std::atomic<uint64_t> &captureCount()
{
    static std::atomic<uint64_t> v{ 0 };
    return v;
}

std::atomic<uint64_t> &captureBytes()
{
    static std::atomic<uint64_t> v{ 0 };
    return v;
}

}  // namespace

void PageAccounting::onPageAllocated( size_t sampleBytes )
{
    livePages().fetch_add( 1, std::memory_order_relaxed );
    everPages().fetch_add( 1, std::memory_order_relaxed );
    const uint64_t now =
        liveBytes().fetch_add( (uint64_t) sampleBytes, std::memory_order_relaxed )
        + (uint64_t) sampleBytes;

    // Raise the high-water mark. A plain compare-and-store would lose races
    // between two allocating workers; the CAS loop keeps the maximum honest
    // without putting a lock on the allocation path.
    uint64_t peak = peakLiveBytes().load( std::memory_order_relaxed );
    while( now > peak
           && !peakLiveBytes().compare_exchange_weak( peak, now,
                                                      std::memory_order_relaxed ) ) {
        // peak was refreshed by compare_exchange_weak; retry.
    }
}

void PageAccounting::onPageReleased( size_t sampleBytes )
{
    livePages().fetch_sub( 1, std::memory_order_relaxed );
    liveBytes().fetch_sub( (uint64_t) sampleBytes, std::memory_order_relaxed );
}

PageMemoryStats PageAccounting::global()
{
    PageMemoryStats s;
    s.pages = livePages().load( std::memory_order_relaxed );
    s.bytes = liveBytes().load( std::memory_order_relaxed );
    return s;
}

uint64_t PageAccounting::everAllocated()
{
    return everPages().load( std::memory_order_relaxed );
}

uint64_t PageAccounting::peakBytes()
{
    return peakLiveBytes().load( std::memory_order_relaxed );
}

void PageAccounting::onPoolReserved( uint64_t pages, uint64_t bytes )
{
    poolPages().fetch_add( pages, std::memory_order_relaxed );
    poolBytes().fetch_add( bytes, std::memory_order_relaxed );
}

void PageAccounting::onPoolFreed( uint64_t pages, uint64_t bytes )
{
    poolPages().fetch_sub( pages, std::memory_order_relaxed );
    poolBytes().fetch_sub( bytes, std::memory_order_relaxed );
}

PageMemoryStats PageAccounting::poolReserved()
{
    PageMemoryStats s;
    s.pages = poolPages().load( std::memory_order_relaxed );
    s.bytes = poolBytes().load( std::memory_order_relaxed );
    return s;
}

void PageAccounting::onCaptureAllocated( size_t bytes )
{
    captureCount().fetch_add( 1, std::memory_order_relaxed );
    captureBytes().fetch_add( (uint64_t) bytes, std::memory_order_relaxed );
}

void PageAccounting::onCaptureReleased( size_t bytes )
{
    captureCount().fetch_sub( 1, std::memory_order_relaxed );
    captureBytes().fetch_sub( (uint64_t) bytes, std::memory_order_relaxed );
}

PageMemoryStats PageAccounting::capturesResident()
{
    PageMemoryStats s;
    s.pages = captureCount().load( std::memory_order_relaxed );
    s.bytes = captureBytes().load( std::memory_order_relaxed );
    return s;
}

void PageAccounting::resetCumulative()
{
    everPages().store( 0, std::memory_order_relaxed );
    // The peak is reseeded to what is resident NOW, not to zero: bytes that are
    // still alive are still a peak that has been reached.
    peakLiveBytes().store( liveBytes().load( std::memory_order_relaxed ),
                           std::memory_order_relaxed );
}

}  // namespace pages
}  // namespace tw
