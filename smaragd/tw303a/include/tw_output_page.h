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

// Forward declaration
class twComponent;

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
struct twOutputPage {
    static const size_t PAGE_SIZE = 256 * 1024;  // 256 kB per page (~0.68s @ 48kHz stereo)
    static const size_t FRAME_CAPACITY = PAGE_SIZE / sizeof(float) / 2;  // stereo frames

    // Time range this page covers (in sample frames)
    uint64_t startPosition;

    // Frozen audio samples for this range
    std::vector<float> samples;

    // Number of valid frames in this page (may be less than FRAME_CAPACITY)
    uint32_t validFrames;

    // Which rendering aspects have been computed for this page
    std::atomic<uint32_t> validAspects;

    // Internal state snapshot (for sequential components like reverbs, delays)
    // Allows resuming rendering from this page's endpoint without losing state
    std::any internalState;

    // Timing for stale-data fallback logic
    std::chrono::steady_clock::time_point createdAt;

    twOutputPage()
        : startPosition(0),
          validFrames(0),
          validAspects(0),
          createdAt(std::chrono::steady_clock::now())
    {
        samples.resize(FRAME_CAPACITY);
    }
};

#endif  // _TW_OUTPUT_PAGE_H_
