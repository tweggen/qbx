#include "tw/pages/capture_page_pool.h"
#include <cassert>

CapturePagePool::CapturePagePool(size_t numPages)
    : pages_(numPages) {
    // Initialize free list with all page indices
    for (size_t i = 0; i < numPages; ++i) {
        freeIndices_.push(i);
    }
}

std::shared_ptr<CapturePageData> CapturePagePool::allocatePage() {
    std::lock_guard<std::mutex> lock(poolLock_);

    // Check if pool has free pages
    if (freeIndices_.empty()) {
        return nullptr;  // Pool exhausted
    }

    // Get next free page index
    size_t idx = freeIndices_.front();
    freeIndices_.pop();

    assert(idx < pages_.size());

    // Return page with custom deleter that returns it to pool
    CapturePageData* page = &pages_[idx];
    return std::shared_ptr<CapturePageData>(page, PageDeleter{this, idx});
}

void CapturePagePool::releasePage(size_t pageIndex) {
    std::lock_guard<std::mutex> lock(poolLock_);

    assert(pageIndex < pages_.size());

    // Clear valid aspects to mark page as empty
    pages_[pageIndex].validAspects = 0;
    pages_[pageIndex].generation = 0;

    // Return to free list
    freeIndices_.push(pageIndex);
}

size_t CapturePagePool::numFreePages() const {
    std::lock_guard<std::mutex> lock(poolLock_);
    return freeIndices_.size();
}

size_t CapturePagePool::numAllocatedPages() const {
    std::lock_guard<std::mutex> lock(poolLock_);
    return pages_.size() - freeIndices_.size();
}
