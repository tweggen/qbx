#ifndef PAGE_INTERFACE_H_
#define PAGE_INTERFACE_H_

#include <cstdint>
#include "tw/core/twtypes.h"
#include <mutex>
#include <atomic>
#include <chrono>
#include <any>
#include <memory>

/**
 * Unified page interface for frozen component output.
 *
 * Phase 4.1: Common abstraction for audio pages (twOutputPage) and
 * capture pages (CapturePageData). Both represent immutable snapshots
 * of rendered output at a specific time position.
 *
 * Design:
 * - Common interface for synchronization and metadata
 * - Subclasses provide storage (samples vector vs raw buffer)
 * - Rendering code works with interface, not concrete types
 */
class PageBase {
public:
    virtual ~PageBase() = default;

    // Synchronization: protects concurrent access from render threads
    virtual std::mutex& getMutex() const = 0;

    // Time position: sample frame where this page's data begins
    virtual offset_t getStartPosition() const = 0;
    virtual void setStartPosition(offset_t pos) = 0;

    // Validity: which aspects (Preview, Playback, Export, etc.) are computed
    virtual uint32_t getValidAspects() const = 0;
    virtual void setValidAspects(uint32_t aspects) = 0;

    // Generation: incremented on invalidation, allows staleness detection
    virtual uint64_t getGeneration() const = 0;
    virtual void incrementGeneration() = 0;

    // Size: total capacity and valid frames
    virtual size_t getPageSize() const = 0;
    virtual uint32_t getValidFrames() const = 0;
    virtual void setValidFrames(uint32_t frames) = 0;

    // Data access: get pointer to raw page data
    virtual void* getDataPtr() = 0;
    virtual const void* getDataPtr() const = 0;

    // Internal state snapshot (for sequential components)
    virtual std::any& getInternalState() = 0;
    virtual const std::any& getInternalState() const = 0;

    // Creation time (for staleness tracking)
    virtual std::chrono::steady_clock::time_point getCreatedAt() const = 0;
};

#endif  // PAGE_INTERFACE_H_
