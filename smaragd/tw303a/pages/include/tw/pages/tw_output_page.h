#ifndef _TW_OUTPUT_PAGE_H_
#define _TW_OUTPUT_PAGE_H_

#include <cstdint>
#include <vector>
#include <memory>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>

// C++17 type erasure for internal state snapshots
#include <any>

#include "tw/pages/page_interface.h"

// Forward declaration
class twComponent;

// Phase 5 Gap 11: Unified page size constant
// All frozen pages (component, SObject) use this size for alignment
// 256 kB provides ~0.68 seconds at 48kHz stereo, balancing:
//   - Cache coherency (fits in L3)
//   - Granularity (fine enough for responsive updates)
//   - Memory overhead (sparse page allocation)
static constexpr size_t FROZEN_PAGE_SIZE_BYTES = 256 * 1024;

// Aspects that can be frozen for a component's output page
enum twRenderAspect : uint32_t {
    twAspectPreview   = 1u << 0,  // Waveform peaks for timeline visualization
    twAspectPlayback  = 1u << 1,  // Real-time audio for playback
    twAspectExport    = 1u << 2,  // High-quality audio for file export
    twAspectMetadata  = 1u << 3,  // Duration, peak levels, analysis data

    twAspectAll       = twAspectPreview | twAspectPlayback | twAspectExport | twAspectMetadata,
};

// Frozen output of a component for a time window
// Stores component output samples + internal state snapshot for sequential rendering
struct twOutputPage : public PageBase {
    // Phase 5 Gap 11: Unified page size
    static constexpr size_t PAGE_SIZE = FROZEN_PAGE_SIZE_BYTES;  // 256 kB per page
    static constexpr size_t FRAME_CAPACITY = PAGE_SIZE / sizeof(float);  // mono frames

    // Time range this page covers (in sample frames)
    uint64_t startPosition;

    // Frozen audio samples for this range
    std::vector<float> samples;

    // Number of valid frames in this page (may be less than FRAME_CAPACITY)
    uint32_t validFrames;

    // Which rendering aspects have been computed for this page
    std::atomic<uint32_t> validAspects;

    // Generation counter to detect invalidation
    // Incremented when invalidateAllPages() invalidates this page.
    // Audio threads compare against their cached generation to detect stale pages.
    // Allows audio to drop references to pages that have been invalidated and repurposed.
    std::atomic<uint64_t> generation{0};

    // Internal state snapshot (for sequential components like reverbs, delays)
    // Allows resuming rendering from this page's endpoint without losing state
    std::any internalState;

    // Timing for stale-data fallback logic
    std::chrono::steady_clock::time_point createdAt;

    // Phase 5 Gap 12: Multi-Consumer Page Locking
    // Protects concurrent access from multiple render threads reading same page.
    // Writers (revalidator) hold this lock while freezing/populating the page.
    // Readers (render loops) acquire this lock when reading internalState or during updates.
    // Lock acquired during freezePage() state capture/restore operations.
    mutable std::mutex pageMutex;

    twOutputPage()
        : startPosition(0),
          validFrames(0),
          validAspects(0),
          generation(0),
          createdAt(std::chrono::steady_clock::now())
    {
        samples.resize(FRAME_CAPACITY);
    }

    // PageBase interface implementation
    std::mutex& getMutex() const override { return pageMutex; }
    uint64_t getStartPosition() const override { return startPosition; }
    void setStartPosition(uint64_t pos) override { startPosition = pos; }
    uint32_t getValidAspects() const override { return validAspects.load(); }
    void setValidAspects(uint32_t aspects) override { validAspects.store(aspects); }
    uint64_t getGeneration() const override { return generation.load(); }
    void incrementGeneration() override { generation.fetch_add(1); }
    size_t getPageSize() const override { return PAGE_SIZE; }
    uint32_t getValidFrames() const override { return validFrames; }
    void setValidFrames(uint32_t frames) override { validFrames = frames; }
    void* getDataPtr() override { return samples.data(); }
    const void* getDataPtr() const override { return samples.data(); }
    std::any& getInternalState() override { return internalState; }
    const std::any& getInternalState() const override { return internalState; }
    std::chrono::steady_clock::time_point getCreatedAt() const override { return createdAt; }
};

#endif  // _TW_OUTPUT_PAGE_H_
