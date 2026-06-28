# Unified Rendering Architecture V2: Gap Analysis

**Date:** 2026-06-27  
**Status:** Current Implementation vs. V2 Concept  
**Scope:** Identify what's missing, divergences, and implementation roadmap

---

## Executive Summary

**Current State:** Zero component-level page caching. All pages managed at SObject (container/SCut) level via `CaptureRevalidator`.

**V2 Requirement:** Pages at every `twComponent`, sequential freezing from reset state, internal state snapshots, push-based rendering.

**Verdict:** Current implementation is production-ready for SObject-level caching but **not suitable for deep component graph optimization**. Implementing V2 is a **major architectural refactor (~40–50 hours)**.

---

## Critical Gaps (Blocking Order)

### Gap 1: Component Output Pages Missing

**Concept Requirement:**  
Each `twComponent` maintains a page cache of its frozen output:
```cpp
struct OutputPage {
    offset_t startPosition;
    std::vector<sample_t> samples;
    std::any internalState;  // Snapshot for sequential components
    uint32_t validAspects;
};

class twComponent {
    std::map<offset_t, std::shared_ptr<OutputPage>> pages_;
    std::mutex pagesMutex_;
    std::shared_ptr<OutputPage> getOrAllocatePage(offset_t pos, uint32_t aspects);
};
```

**Current Implementation:**  
- `CapturePagePool` + `CapturePageData` exist at **SObject level only**
- `twComponent` base class has **no pages_ member**, **no OutputPage struct**
- Pages cache at container level (one page per SCut/track), not at component level

**Divergence:**  
Concept needs pages per signal path. Current: pages per container.

**Risk:** **CRITICAL**  
This is the foundational abstraction. Without it, no freezing model.

**Effort:** **MAJOR (> 16 hours)**
- Add `pages_`, `pagesMutex_` to `twComponent`
- Add `OutputPage` struct with state snapshot
- Implement `getOrAllocatePage()`, `releaseOldPages()` in base class
- Update 50+ subclasses for state snapshot support

**Blockers:** Gaps 2, 3, 4, 5, 6

---

### Gap 2: Internal State Snapshots Not Implemented

**Concept Requirement:**  
Sequential components (reverbs, delays) capture/restore state:
```cpp
// In OutputPage
std::any internalState;

// In twComponent
virtual std::any captureInternalState() = 0;
virtual void restoreInternalState(const std::any&) = 0;
```

**Current Implementation:**  
- `CapturePageData` has **no internalState field**
- Components maintain state (e.g., `twSampleReader::pos_`) but never snapshot it
- No serialization mechanism for delay lines, feedback buffers, grain state

**Divergence:**  
Concept expects stateful component design. Current: state is implicit, never captured.

**Risk:** **CRITICAL**  
Without snapshots, sequential rendering fails. Cannot freeze page 2 without losing state from page 1.

**Effort:** **MAJOR (> 16 hours)**
- Add `internalState` to `OutputPage`
- Add capture/restore virtuals to `twComponent`
- Implement in every stateful component:
  - twSampleReader: snapshot `pos_`
  - twGrainSource: snapshot playhead + grain state
  - Any future reverb/delay: snapshot delay lines

**Blockers:** Gaps 3, 6

---

### Gap 3: Sequential Freezing Loop (`freezePage()`) Not Implemented

**Concept Requirement:**  
```cpp
std::shared_ptr<OutputPage> twComponent::freezePage(
    offset_t startPos,
    const std::vector<sample_t>& inputData,  // Pre-frozen from upstream
    int sampleRate
) {
    if (startPos == 0) reset();
    else restoreInternalState(pages_[startPos - PAGE_SIZE]->internalState);
    
    page->samples.resize(PAGE_SIZE);
    length_t rendered = 0;
    while (rendered < PAGE_SIZE) {
        length_t got = renderFrames(page->samples.data() + rendered, toRender, input);
        rendered += got;
    }
    page->internalState = captureInternalState();
    return page;
}
```

**Current Implementation:**  
- `twComponent` has **no freezePage() method**
- Components expose only `calcOutputTo()` for pull-based rendering
- No concept of "rendering from reset state" or sequential advancement

**Divergence:**  
Concept: push-based freezing (producer writes pages). Current: pull-based (consumer calls calcOutputTo).

**Risk:** **CRITICAL**  
Without freezePage(), entire caching model collapses.

**Effort:** **MAJOR (> 16 hours)**
- Add `freezePage()` virtual to `twComponent`
- Adapt from `calcOutputTo()` logic
- Implement in all ~50 component types
- Update `CaptureRevalidator` to call `freezePage()` instead of `calcOutputTo()`

**Blockers:** Gaps 4, 5, 6

---

### Gap 4: Render Loops Still Use `seekTo()` + `calcOutputTo()`

**Concept Requirement:**  
Render loops read from pre-frozen pages, NO seekTo:
```cpp
for (offset_t pos = 0; pos < totalFrames; pos += PAGE_SIZE) {
    auto page = component->getOrAllocatePage(pos, PLAYBACK);
    if (page && page->validAspects & PLAYBACK) {
        copyToOutput(page->samples, output);  // No seekTo()
    }
}
```

**Current Implementation:**  
- **twspeaker.cc:** `audioEngine->seekTo()` → `pullBlock()` → `synthOutput->seekTo()`
- **render_session.cc:** `audioEngine->seekTo()` at start, then `pullBlock()`
- **audio_engine.cc:** Calls `synthOutput->seekTo()` when looping
- **twtrackmix.cc:** Calls `seekTo()` per position jump

**Divergence:**  
Concept: no seekTo in render loops. Current: seekTo is primary synchronization.

**Risk:** **HIGH** (Correctness + Efficiency)
- Correctness: Calling seekTo on reverb mid-stream loses delay lines
- Efficiency: Two SCuts reading same reverb trigger two separate calcOutputTo calls (redundant)

**Effort:** **MAJOR (> 16 hours)**
- Complete Phases A–D of implementation roadmap
- Refactor AudioEngine to read from frozen pages
- Remove seekTo from playback/export loops
- Test: no regression in render quality

**Blockers:** Gaps 2, 3

---

### Gap 5: Multiple Indirect SCuts Referencing Same Component (Redundancy)

**Concept Requirement:**  
Two SCuts reading same reverb at overlapping times both read same frozen pages:
```
SCut A (time 0:00–0:05) → Reverb
SCut B (time 0:02–0:07) → Reverb

Reverb.frozenPages[0..N] computed once
  → A reads [0–5], B reads [2–7]
  → No redundant computation
```

**Current Implementation:**  
- Each SCut owns a `twSampleReader` instance (per-consumer reader)
- If two SCuts read same sample, each has independent reader
- No shared frozen output; each reader computes independently

**Divergence:**  
Current: per-consumer state (avoids races, creates redundancy). Concept: shared frozen pages (no redundancy, requires sync).

**Risk:** **HIGH** (Efficiency)
- Not broken, but loses CPU savings of caching
- Two overlapping indirect SCuts on reverb = 2× DSP load (could be 1×)

**Effort:** **MEDIUM (4–16 hours)**
- Resolves automatically once Gaps 1, 2, 3 implemented
- Revalidator freezes component once; both SCuts read same pages

**Blockers:** None (efficiency only)

---

### Gap 6: No Component-Level Reset State

**Concept Requirement:**  
```cpp
class twComponent {
    virtual void reset() = 0;
};
```

**Current Implementation:**  
- `twComponent` has **no virtual reset()**
- Only `twPlugin` (in plugins/) declares `virtual void reset()`
- Most components in tw303a don't implement reset

**Divergence:**  
Concept: reset is fundamental interface. Current: no consistent contract.

**Risk:** **HIGH** (Blocking)
- Cannot freeze first page without reset()

**Effort:** **MEDIUM (4–16 hours)**
- Add `virtual void reset() = 0` to `twComponent`
- Implement in all subclasses:
  - Most: empty or simple state clear
  - twSampleReader: seek to start, clear buffer
  - twGrainSource: reset playhead

**Blockers:** Gap 3

---

## High-Risk Gaps (Correctness)

### Gap 7: No reset() Virtual Method in twComponent Base Class

**Concept:** Pure virtual in base  
**Current:** Not defined  
**Risk:** HIGH (blocks freezing)  
**Effort:** QUICK (< 4 hours)

---

### Gap 8: No renderFrames() Method (Push-Based Render)

**Concept Requirement:**  
```cpp
length_t renderFrames(sample_t* output, length_t N, const sample_t* input);
```

Renders with pre-prepared input (not read from latches), advances state.

**Current Implementation:**  
- Only `calcOutputTo()` exists (pull-based, reads from latches)
- No variant for push-based rendering with prepared input

**Divergence:**  
Concept needs push-based (here's your input, produce output). Current: pull-based.

**Risk:** **HIGH** (Blocking)
- Essential for freezing (must feed pre-frozen input to next stage)

**Effort:** **MAJOR (> 16 hours)**
- Add `virtual length_t renderFrames(...)` to `twComponent`
- Implement in all subclasses (adapt from `calcOutputTo()`)
- Major API change; requires careful planning

**Blockers:** Gap 3

---

## Medium-Risk Gaps (Partial Correctness)

### Gap 9: Component Invalidation Cascade Not Implemented

**Concept Requirement:**  
Parameter change invalidates component's pages, propagates to parents:
```cpp
void twSpringReverb::setDecayTime(float newDecay) {
    decayTime_ = newDecay;
    invalidateAllPages();
    notifyDependentsChanged();  // Propagate to parents
}
```

**Current Implementation:**  
- Invalidation only at SObject level
- No cascade for multi-level component hierarchies

**Divergence:**  
Concept: component + parent cascade. Current: SObject only.

**Risk:** **MEDIUM** (Partial Correctness)
- Works for simple 1:1 SObject→component mappings
- Breaks for multi-component trees (mixer with N inputs)

**Effort:** **MEDIUM (4–16 hours)**
- Add `invalidateAllPages()` to twComponent
- Add `notifyDependentsChanged()` for parent propagation
- Call during all parameter changes

**Blockers:** None

---

### Gap 10: CaptureRevalidator at SObject Level, Not Component Level

**Concept Requirement:**  
Revalidator schedules component page freezing with dependency tracking:
```cpp
void freezeComponentPage(twComponent* comp, offset_t pos, uint32_t aspects) {
    auto inputs = comp->getInputPages(pos);
    if (!allReady(inputs)) return;  // Wait for upstream
    comp->freezePage(pos, inputs, aspects);
}
```

**Current Implementation:**  
- Jobs hold `SObject*`, call `object->recomputePlayback(page)`
- No component-level job type
- No dependency tracking between component pages

**Divergence:**  
Concept: component + dependency jobs. Current: SObject jobs only.

**Risk:** **MEDIUM** (Partial Correctness)
- Works for SObject→component 1:1
- Breaks for component DAG (which component freezes when?)

**Effort:** **MEDIUM (4–16 hours)**
- Define `CaptureComponentFreezingJob` struct
- Implement dependency tracking
- Extend revalidator job dispatch

**Blockers:** None

---

## Low-Risk Gaps (Design Details)

### Gap 11: No Page Size Concept in Components

**Concept:** All components align to consistent page size (256 kB)  
**Current:** Pages at SObject level only  
**Risk:** LOW (not correctness)  
**Effort:** QUICK (< 4 hours)

---

### Gap 12: No Multi-Consumer Page Locking

**Concept:** Mutex + shared_ptr for thread-safe reads  
**Current:** Partial (SObject level only)  
**Risk:** LOW (already sound model)  
**Effort:** QUICK (< 4 hours)

---

## Summary Table

| # | Gap | Risk | Effort | Blocker For |
|----|-----|------|--------|-----------|
| **1** | **Component Output Pages** | **CRITICAL** | **MAJOR** | 2,3,4,5,6 |
| **2** | **Internal State Snapshots** | **CRITICAL** | **MAJOR** | 3,6 |
| **3** | **freezePage() Method** | **CRITICAL** | **MAJOR** | (core blocker) |
| **4** | **seekTo() in Render Loops** | **HIGH** | **MAJOR** | (efficiency) |
| **5** | **Multiple Indirect SCuts** | **HIGH** | **MEDIUM** | (efficiency) |
| **6** | **Component Reset State** | **HIGH** | **MEDIUM** | 3 |
| 7 | reset() Virtual Method | HIGH | QUICK | 3,6 |
| 8 | renderFrames() Method | HIGH | MAJOR | 3 |
| 9 | Invalidation Cascade | MEDIUM | MEDIUM | — |
| 10 | Component-Level Freezing Jobs | MEDIUM | MEDIUM | — |
| 11 | Page Size Alignment | LOW | QUICK | — |
| 12 | Multi-Consumer Locking | LOW | QUICK | — |

---

## Implementation Roadmap

**Phase 1: Foundations (CRITICAL)**
- [ ] Gap 7: Add `reset()` virtual to `twComponent`
- [ ] Gap 1: Add component-level page cache infrastructure
- [ ] Gap 2: Add internal state snapshot mechanism to `OutputPage`

**Phase 2: Freezing Engine (CRITICAL)**
- [ ] Gap 8: Add `renderFrames()` push-based method
- [ ] Gap 3: Implement `freezePage()` using reset + renderFrames
- [ ] Gap 6: Verify all components implement reset()

**Phase 3: Render Path Refactoring (CRITICAL)**
- [ ] Gap 4: Refactor playback/export loops to read frozen pages (no seekTo)
- [ ] Test: verify output matches Phase 4 quality

**Phase 4: Optimizations & Extensions (MEDIUM)**
- [ ] Gap 9: Add invalidation cascade
- [ ] Gap 10: Extend revalidator to component-level freezing jobs
- [ ] Gap 5: Verify multi-consumer page sharing works

**Phase 5: Polish (LOW)**
- [ ] Gap 11: Align page sizes
- [ ] Gap 12: Add multi-consumer locking
- [ ] Comprehensive testing & benchmarking

---

## Cost Estimates

| Phase | Effort | Risk | Payoff |
|-------|--------|------|--------|
| Phase 1 | 20 hours | Very High (foundation) | Enables everything |
| Phase 2 | 16 hours | High (new API) | Core freezing |
| Phase 3 | 12 hours | High (refactor) | Correctness + efficiency |
| Phase 4 | 8 hours | Medium | Optimization |
| Phase 5 | 4 hours | Low | Polish |
| **Total** | **~60 hours** | — | **Complete V2 implementation** |

---

## Conclusion

The current implementation is **production-ready for SObject-level caching** (SCuts, tracks, groups) but **not equipped for component-level optimization**. 

Implementing V2 would require a **~60-hour architectural refactor**, prioritizing:
1. ✅ Component page cache infrastructure (Gap 1)
2. ✅ Internal state snapshots (Gap 2)  
3. ✅ Sequential freezing with reset/restore (Gaps 3, 6, 7, 8)
4. ✅ Remove seekTo from render loops (Gap 4)
5. ✅ Component-level invalidation (Gap 9)
6. ✅ Revalidator with dependency tracking (Gap 10)

**Payoff:** Deterministic, efficient rendering of arbitrarily deep component graphs; correct sequential DSP; no redundant computation for overlapping indirect SCuts.
