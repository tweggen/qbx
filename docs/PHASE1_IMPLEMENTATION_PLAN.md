# Phase 1 Implementation Plan: Component-Level Page Caching Foundations

**Status:** Ready to implement  
**Date:** 2026-06-27  
**Scope:** Gaps 7, 1, 2 (3 critical blockers for unified rendering)  
**Effort:** ~36 hours total  

---

## Overview

Phase 1 establishes the foundational infrastructure for component-level page caching:

1. **Gap 7:** Add `reset()` virtual method to `twComponent` base class (~4 hours)
2. **Gap 1:** Add page cache infrastructure to `twComponent` (~16 hours)
3. **Gap 2:** Add internal state snapshot mechanism to `OutputPage` (~16 hours)

These three work together:
- `reset()` puts components in known initial state
- Page cache stores output + internal state
- State snapshots enable sequential rendering (next page resumes from prior page's state)

---

## Gap 7: Add reset() Virtual Method

### Files to Modify

**`tw303a/include/tw.h`** (or appropriate base class header)

```cpp
class twComponent {
    // ... existing members ...
    
    // NEW: Pure virtual reset method
    // Reset component to initial state (silence, zero position, empty buffers)
    virtual void reset() = 0;
};
```

### Implementation Strategy

**Step 1: Add declaration to base class** (1 hour)
- Add `virtual void reset() = 0;` to `twComponent`

**Step 2: Add implementations to all subclasses** (2 hours)
- Stateless components (oscillators, basic mixers): empty implementation or `{}`
- `twSampleReader`: seek to sample start, clear reader position
- `twGrainSource`: reset playhead to start
- `twTrackMix`: reset play offset
- `twPlugin`: already exists in plugin headers, ensure consistency
- Any other stateful components: reset internal buffers/state

**Step 3: Verify compilation** (1 hour)
- Build full codebase
- Fix any missing implementations
- Ensure no circular dependencies

### Testing

- ✅ Compiles without errors
- ✅ All components implement `reset()` (grep for "twComponent.*:" to find subclasses)
- ✅ Can call `component->reset()` on any twComponent without crashes

### Files Touched

- `tw303a/include/tw.h` (or `twcomponent.h` if separate)
- `tw303a/src/tw*.cc` (all component implementations)
- ~50 subclass files total

---

## Gap 1: Component-Level Page Cache Infrastructure

### New Data Structures

**File: `tw303a/include/tw_output_page.h`** (NEW)

```cpp
#pragma once

#include <cstdint>
#include <vector>
#include <chrono>
#include <any>
#include <memory>

namespace tw {

// Frozen output of a component for a time range
struct OutputPage {
    static const size_t PAGE_SIZE = 256 * 1024;  // 256 kB per page
    static const size_t FRAME_CAPACITY = PAGE_SIZE / sizeof(float) / 2;  // stereo
    
    // Time range this page covers
    offset_t startPosition = 0;
    
    // Frozen audio samples for this range
    std::vector<float> samples;  // Size: FRAME_CAPACITY or less
    uint32_t validFrames = 0;    // How many frames are actually valid
    
    // Internal state snapshot (for sequential components like reverbs)
    // Allows resuming from this page's endpoint
    std::any internalState;
    
    // Which rendering aspects are complete for this page
    uint32_t validAspects = 0;  // Bitmask: Preview | Playback | Export | Metadata
    
    // Timing for stale-data fallback
    std::chrono::steady_clock::time_point createdAt;
    
    // Refcount (shared_ptr handles this)
};

// Aspects bitmask
enum RenderAspect : uint32_t {
    Preview   = 1u << 0,  // Waveform peaks for timeline
    Playback  = 1u << 1,  // Real-time audio output
    Export    = 1u << 2,  // High-quality export audio
    Metadata  = 1u << 3,  // Duration, peak levels
    
    All       = Preview | Playback | Export | Metadata,
};

}  // namespace tw
```

### Modifications to twComponent Base Class

**File: `tw303a/include/tw.h`** (modify existing `twComponent`)

```cpp
class twComponent {
    // ... existing members ...
    
private:
    // NEW: Page cache for component output
    std::map<offset_t, std::shared_ptr<tw::OutputPage>> pages_;
    mutable std::mutex pages_mutex_;
    
public:
    // Get or allocate a page covering the requested range
    // Non-blocking: returns nullptr if page not yet frozen, calls are non-blocking
    std::shared_ptr<tw::OutputPage> getOrAllocatePage(
        offset_t startPos,
        uint32_t aspectsMask = tw::RenderAspect::All
    );
    
    // Release pages outside of a retention window (memory management)
    void releaseOldPages(offset_t keepAfterPos);
    
    // Get all pages in a time range (for iteration/cleanup)
    std::vector<std::shared_ptr<tw::OutputPage>> getPagesInRange(
        offset_t startPos,
        offset_t endPos
    ) const;
    
    // Invalidate all cached pages (called when parameters change)
    void invalidateAllPages();
    
    // Internal: mark a specific page as frozen and valid
    void setPageAsFrozen(offset_t startPos, std::shared_ptr<tw::OutputPage> page);
};
```

### Implementation Details

**`tw303a/src/tw.cc`** (or appropriate implementation file)

```cpp
std::shared_ptr<tw::OutputPage> twComponent::getOrAllocatePage(
    offset_t startPos,
    uint32_t aspectsMask
) {
    std::lock_guard lock(pages_mutex_);
    
    auto it = pages_.find(startPos);
    if (it != pages_.end()) {
        // Page exists
        if (it->second->validAspects & aspectsMask) {
            // Page is already frozen for requested aspects
            return it->second;
        }
        // Page exists but not for these aspects; return it (incomplete)
        return it->second;
    }
    
    // Allocate new page
    auto page = std::make_shared<tw::OutputPage>();
    page->startPosition = startPos;
    page->createdAt = std::chrono::steady_clock::now();
    page->samples.resize(tw::OutputPage::FRAME_CAPACITY);
    pages_[startPos] = page;
    
    // Return (not yet frozen; consumer will retry or use stale fallback)
    return page;
}

void twComponent::releaseOldPages(offset_t keepAfterPos) {
    std::lock_guard lock(pages_mutex_);
    
    for (auto it = pages_.begin(); it != pages_.end(); ) {
        if (it->first + tw::OutputPage::PAGE_SIZE < keepAfterPos) {
            it = pages_.erase(it);
        } else {
            ++it;
        }
    }
}

std::vector<std::shared_ptr<tw::OutputPage>> twComponent::getPagesInRange(
    offset_t startPos,
    offset_t endPos
) const {
    std::lock_guard lock(pages_mutex_);
    std::vector<std::shared_ptr<tw::OutputPage>> result;
    
    for (auto& [pos, page] : pages_) {
        if (pos >= startPos && pos < endPos) {
            result.push_back(page);
        }
    }
    return result;
}

void twComponent::invalidateAllPages() {
    std::lock_guard lock(pages_mutex_);
    for (auto& [pos, page] : pages_) {
        page->validAspects = 0;  // Mark all aspects as stale
    }
}

void twComponent::setPageAsFrozen(
    offset_t startPos,
    std::shared_ptr<tw::OutputPage> page
) {
    std::lock_guard lock(pages_mutex_);
    pages_[startPos] = page;
}
```

### Files to Create/Modify

- **NEW:** `tw303a/include/tw_output_page.h`
- **MODIFY:** `tw303a/include/tw.h` (add page cache members + methods)
- **MODIFY:** `tw303a/src/tw.cc` (implement page cache methods)
- **MODIFY:** `tw303a/CMakeLists.txt` (add new header if needed)

### Testing

- ✅ Compiles without errors
- ✅ Can allocate a page: `component->getOrAllocatePage(0)`
- ✅ Can retrieve same page twice: returns same shared_ptr
- ✅ Can release old pages: `releaseOldPages(1000)` frees pages before 1000
- ✅ Can invalidate: `invalidateAllPages()` marks pages as stale
- ✅ Thread-safety: concurrent `getOrAllocatePage()` calls don't deadlock

---

## Gap 2: Internal State Snapshot Mechanism

### OutputPage Modifications

**Already done in Gap 1 above:** `OutputPage::internalState` field (type `std::any`)

### twComponent Interface for State Management

**File: `tw303a/include/tw.h`** (add to `twComponent`)

```cpp
class twComponent {
    // ... existing + Gap 1 members ...
    
public:
    // Capture current internal state (for serialization into OutputPage)
    // Default: return empty any (stateless components)
    // Override: in stateful components (reverbs, delays, etc.)
    virtual std::any captureInternalState() const {
        return std::any();  // Default: no state
    }
    
    // Restore internal state from snapshot (for sequential rendering resume)
    // Default: no-op (stateless components)
    // Override: in stateful components
    virtual void restoreInternalState(const std::any& state) {
        // Default: no-op
    }
};
```

### Implementation for Stateless Components

Most components in tw303a are stateless. For these, no override needed—default is fine.

### Implementation for Stateful Components

**Example: `twSampleReader`** (if it needs state snapshots)

```cpp
// In twsamplereader.h
class twSampleReader : public twComponent {
    // ... existing members ...
    
public:
    std::any captureInternalState() const override;
    void restoreInternalState(const std::any& state) override;
};

// In twsamplereader.cc
std::any twSampleReader::captureInternalState() const {
    struct State {
        offset_t pos;
        // Other reader-specific state
    };
    return std::any(State{pos_, /* ... */});
}

void twSampleReader::restoreInternalState(const std::any& state) {
    try {
        auto s = std::any_cast<State>(state);
        pos_ = s.pos;
        // Restore other state
    } catch (const std::bad_any_cast&) {
        // State format mismatch; log warning, continue
    }
}
```

### Files to Modify

- **MODIFY:** `tw303a/include/tw.h` (add capture/restore virtuals)
- **MODIFY:** `tw303a/src/tw.cc` (add default implementations)
- **MODIFY:** Component subclasses as needed (implement for stateful types)

### Stateful Components Needing Override

Priority list (implement first):
1. `twSampleReader` (if position needs snapshot)
2. `twGrainSource` (if grain state needs snapshot)
3. Any future reverbs/delays
4. `twPlugin` base class (ensure plugins can snapshot)

Lower priority:
- Simple oscillators (no state needed)
- Basic mixers (no state needed)

### Testing

- ✅ Default: `component->captureInternalState()` returns empty `std::any`
- ✅ Stateless component: capture/restore is no-op
- ✅ Stateful component: capture → restore → verify state matches
- ✅ Mismatch handling: bad cast doesn't crash (catch & log)

---

## Implementation Order (Dependency Chain)

1. **Gap 7 first** (1 day)
   - Add `reset()` virtual
   - All components implement (mostly empty)
   - Quick confidence builder

2. **Gap 1 second** (2–3 days)
   - Add `OutputPage` struct
   - Add page cache to `twComponent`
   - Implement `getOrAllocatePage()`, `releaseOldPages()`, etc.
   - Don't call freeze yet, just allocate/retrieve

3. **Gap 2 third** (2–3 days)
   - Add `captureInternalState()` / `restoreInternalState()`
   - Implement for stateful components (sample reader, grain source)
   - Test capture/restore cycle

### Parallel Work Possible

- Gap 7 and Gap 1 can overlap (both are base class changes)
- Gap 2 depends on Gap 1 (needs `OutputPage` defined)

### Timeline Estimate

- **Gap 7:** 4 hours (1 day)
- **Gap 1:** 16 hours (2–3 days, includes testing)
- **Gap 2:** 16 hours (2–3 days, includes testing)
- **Total Phase 1:** ~36 hours (~1 week at full-time, ~2 weeks part-time)

---

## Testing Strategy

### Unit Tests (Per Gap)

**Gap 7 Testing:**
```cpp
TEST(twComponentReset, AllComponentsImplementReset) {
    auto osc = new twOscillator();
    auto mixer = new twStdMixer();
    auto reader = new twSampleReader();
    
    // Should not crash
    osc->reset();
    mixer->reset();
    reader->reset();
    
    delete osc; delete mixer; delete reader;
}
```

**Gap 1 Testing:**
```cpp
TEST(OutputPageCache, AllocateAndRetrieve) {
    auto comp = new twOscillator();
    
    auto page1 = comp->getOrAllocatePage(0);
    auto page2 = comp->getOrAllocatePage(0);
    
    EXPECT_EQ(page1, page2);  // Same page
    EXPECT_EQ(page1->startPosition, 0);
}

TEST(OutputPageCache, ReleasesOldPages) {
    auto comp = new twOscillator();
    
    comp->getOrAllocatePage(0);
    comp->getOrAllocatePage(PAGE_SIZE);
    comp->getOrAllocatePage(2 * PAGE_SIZE);
    
    comp->releaseOldPages(PAGE_SIZE);  // Keep pages after 1×PAGE_SIZE
    
    auto retained = comp->getPagesInRange(PAGE_SIZE, 3 * PAGE_SIZE);
    EXPECT_EQ(retained.size(), 2);  // Two pages retained
}
```

**Gap 2 Testing:**
```cpp
TEST(InternalStateSnapshot, CaptureAndRestore) {
    auto reader = new twSampleReader();
    
    auto state1 = reader->captureInternalState();
    reader->restoreInternalState(state1);
    
    // No crash, restore succeeds
}
```

### Integration Tests

**After all Phase 1 done:**
- ✅ Can allocate pages for multiple components in a simple chain
- ✅ Can reset a component chain and verify pages reflect new state
- ✅ Render quality unchanged (comparison against Phase 4 render)

---

## Known Risks & Mitigation

| Risk | Mitigation |
|------|-----------|
| `std::any` overhead (type safety) | Use sparingly; only for per-component state, not hot path |
| Page cache memory bloat | Start small; aggressive `releaseOldPages()` policy |
| State snapshot serialization complexity | Use simple structs; avoid complex nested types initially |
| Threading contention on `pages_mutex_` | Profile before optimizing; contention unlikely (allocations are sparse) |
| Compilation breakage (50+ subclasses) | Incremental: implement one component, build, iterate |

---

## Success Criteria

- ✅ Gap 7: `twComponent::reset()` declared pure virtual; all subclasses implement
- ✅ Gap 1: Page cache infrastructure in place; allocate, retrieve, release pages
- ✅ Gap 2: Internal state snapshots can be captured/restored; no type errors
- ✅ **No regression:** Existing render quality unchanged (Phase 4 comparison)
- ✅ **Compilation:** Full codebase builds without errors or warnings
- ✅ **Memory safety:** No leaks detected under valgrind/asan
- ✅ **Threading:** No deadlocks or races (ThreadSanitizer clean)

---

## Next Steps (Phase 2)

Once Phase 1 is complete:
- Gap 8: Add `renderFrames()` push-based rendering method
- Gap 3: Implement `freezePage()` sequential freezing loop
- Gap 6: Verify all components implement `reset()` correctly
- Phase 2 depends entirely on Phase 1 being solid

---

## Files Summary

### New Files
- `tw303a/include/tw_output_page.h` — OutputPage struct + enums

### Modified Files
- `tw303a/include/tw.h` — Add reset(), page cache, state snapshot interface
- `tw303a/src/tw.cc` — Implement page cache methods
- `tw303a/src/tw*.cc` — Add reset() implementations
- `tw303a/CMakeLists.txt` — Add new header to build

### Total Changes
- ~3 new files
- ~10 modified files (mostly component implementations)
- ~400 lines of new code
- ~50 simple implementations (mostly `{}` or `return std::any()`)

