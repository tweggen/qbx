# Unified Rendering Architecture V2: Freezing Wires

**Status:** Concept & Design  
**Date:** 2026-06-27  
**Revision:** 2.0 (replaces UNIFIED_RENDERING_ARCHITECTURE.md)  
**Author:** Timo Weggen, Claude  
**Scope:** Cache-based component output freezing for efficient, correct handling of sequential DSP, indirect SCuts, and multi-consumer rendering

---

## Executive Summary

**Core Principle:** Every wire (signal path) between twComponents is "frozen" into pages—rendering each signal once from a well-defined reset state, storing the output, and allowing all consumers to read from that frozen cache.

This model:
- ✅ Eliminates redundant component computation (same signal computed once)
- ✅ Correctly handles sequential/stateful components (reverbs, delays, convolution) with natural sequential rendering
- ✅ Safely handles multiple indirect SCuts referencing the same component (all read same frozen output)
- ✅ Manages memory via page allocation/deallocation (freezing is windowed, not global)
- ✅ Preserves determinism (reset state + same input = same output, always)

**Analogy:** Like "freezing" a track in a DAW—compute it once, store the result, consumers read the frozen output.

---

## Problem Statement (V1 Gaps)

V1 (UNIFIED_RENDERING_ARCHITECTURE.md) proposed a page cache but left critical issues unaddressed:

1. **Sequential Components:** Components like Spring Reverb, Delay, Convolution cannot render arbitrary time windows—they must render sequentially from a known state. V1's "arbitrary-position pull" breaks this.

2. **Multiple SCuts Referencing Same Component:** Two indirect SCuts pointing to the same reverb at overlapping times face an ambiguity: which input feeds the reverb? What is the frozen state supposed to represent?

3. **"One State at a Time" Principle:** A twComponent can only be in one state. But if two threads call seekTo() on the same component, or one revalidator thread freezes while another plays, state corruption is inevitable.

4. **Redundant Computation:** If three SCuts all reference the same delay component, does each trigger its own computation? Or is there one canonical frozen output?

**V2 Answer:** Freeze the **wires** (signal paths), not the components' ability to be queried. The output of every component for a given time range is computed once and cached.

---

## Core Concept: Freezing Wires

### What Does "Freezing" Mean?

In a DAW, "freezing" a track means:
1. Render the track's DSP from start to end, writing output to a file
2. Replace real-time computation with playback of the frozen file
3. Result: same audio, lower CPU, but no longer live/interactive

**Freezing a Wire in Smaragd:**

```
Component Graph (simplified):
  Input ──→ Spring Reverb ──→ Output

Freeze the wire "Spring Reverb → Output":
1. Reset reverb to initial state (empty delay lines)
2. Feed reverb the input signal sequentially from position 0
3. Store reverb's output in a page cache
4. Mark as "frozen from position 0 to N"
5. All consumers read from the frozen page
6. Later: freeze next page [N..M] using state at position N
```

### Why Pages?

Freezing the entire signal path from start-of-project to end would:
- Allocate massive buffers (unnecessary for scrubbing, skipping, etc.)
- Force re-computation of everything on any parameter change
- Break memory budgets

**Pages allow windowed freezing:**
- Page covers time range [startPos..startPos + pageSize] (e.g., ~0.68s @ 48kHz, 256 kB)
- Pages can be allocated on-demand
- Pages can be evicted when no longer needed
- Multiple pages cover a continuous signal

---

## The Freezing Model

### 1. Component Reset State

Every twComponent has a well-defined reset state:

```cpp
class twComponent {
    // Reset to initial state (silence, empty buffers, position 0)
    virtual void reset() = 0;
    
    // Render N frames from current internal state
    // Advances state by N frames internally
    virtual length_t renderFrames(sample_t* output, length_t N) = 0;
};
```

**Examples:**
- `twOscillator::reset()` → sets phase to 0
- `twSampleReader::reset()` → seeks to sample start, clears reader buffer
- `twSpringReverb::reset()` → clears delay lines, dampening state
- `twGrainSource::reset()` → resets grain playhead

### 2. Component Output Pages

Each component maintains a **page cache of its frozen output:**

```cpp
class twComponent {
    struct OutputPage {
        offset_t startPosition;           // Page covers [startPos..startPos+pageSize)
        std::vector<sample_t> samples;    // Frozen output for this range
        
        // For sequential components: internal state snapshot at startPos
        // (Allows resuming render from page boundary)
        std::any internalState;           // e.g., reverb delay lines at frame 0
        
        std::chrono::steady_clock::time_point createdAt;
        uint32_t validAspects;            // Preview/Playback/Export
    };
    
    // Page cache: covers potentially multiple time windows
    std::map<offset_t, std::shared_ptr<OutputPage>> pages_;
    std::mutex pagesMutex_;
    
    // Get or allocate a page covering requested range
    std::shared_ptr<OutputPage> getOrAllocatePage(offset_t startPos, uint32_t aspects);
    
    // Release pages outside a time window (memory cleanup)
    void releaseOldPages(offset_t keepAfterPos);
};
```

### 3. Sequential Rendering: Freeze a Page

To compute a page, freeze the component's output sequentially:

```cpp
std::shared_ptr<OutputPage> twComponent::freezePage(
    offset_t startPos, 
    const std::vector<sample_t>& inputData,
    int sampleRate
) {
    auto page = std::make_shared<OutputPage>();
    page->startPosition = startPos;
    
    // If this is the first page: start from reset state
    // Otherwise: load internal state from previous page's snapshot
    if (startPos == 0) {
        reset();
    } else {
        auto prevPage = pages_[startPos - PAGE_SIZE];
        if (prevPage) {
            restoreInternalState(prevPage->internalState);
        } else {
            // Previous page not in cache; need to recompute
            // This is an error in page management—backfill if needed
            reset();
        }
    }
    
    // Render sequentially: feed input, capture output
    page->samples.resize(PAGE_SIZE);
    length_t rendered = 0;
    
    while (rendered < PAGE_SIZE && inputData.size() > startPos + rendered) {
        length_t toRender = std::min(PAGE_SIZE - rendered, inputData.size() - startPos - rendered);
        
        // Render the component with current input
        // Component's internal state advances automatically
        length_t got = renderFrames(
            page->samples.data() + rendered,
            toRender,
            inputData.data() + startPos + rendered  // Input from upstream
        );
        rendered += got;
    }
    
    // Save internal state for next page's resumption
    page->internalState = captureInternalState();
    
    return page;
}
```

### 4. Consumers Read Frozen Pages

SCuts and other consumers don't call seekTo()/calcOutputTo(). Instead, they read from frozen pages:

```cpp
// In render loop (playback or export):
for (offset_t pos = 0; pos < totalFrames; pos += PAGE_SIZE) {
    // Get frozen page for this component at this position
    auto page = component->getOrAllocatePage(pos, PLAYBACK);
    
    if (!page || !(page->validAspects & PLAYBACK)) {
        // Not yet frozen; schedule async freezing
        revalidator->scheduleFreezing(component, pos, PLAYBACK);
        
        // Fallback: use stale page or silence
        if (stalePage) {
            copyToOutput(stalePage->samples, output, PAGE_SIZE);
        } else {
            memset(output, 0, PAGE_SIZE * sizeof(sample_t));
        }
    } else {
        // Page is frozen; read directly
        copyToOutput(page->samples, output, PAGE_SIZE);
    }
}
```

---

## Handling Multiple Indirect SCuts

### Scenario: Two SCuts Reference Same Component

```
Project:
├─ SCut A (times 0:00–0:05, references Sample X at offset [0–10])
├─ SCut B (times 0:02–0:07, references Sample X at offset [5–15])
└─ Component Graph: Sample X → Spring Reverb → Output

Render timeline 0:00–0:10:
```

### The Key: What is the "Input"?

**Question:** The reverb's frozen output depends on its input. Are A and B mixed before the reverb? Or does the reverb see them separately?

**Answer:** Defined by the component graph structure.

**Case 1: Parallel Feeds (Both A and B Feed the Same Reverb)**

```
Timeline:
A ──┐
    ├─→ Mixer ──→ Spring Reverb ──→ Output
B ──┘

Freeze process:
1. Freeze Mixer output (mixes A + B) for each page [0..N]
   → Mixer.frozenOutput[0..N] = blend of A and B
2. Freeze Reverb input (frozen Mixer output)
   → Reverb.freezePage(page, Mixer.frozenOutput[0..N])
3. Reverb sequentially processes the frozen mix
   → Reverb output page represents reverb response to mix
4. SCuts A and B both read Reverb's frozen output
   → Both get the same reverb tail (correct: they're in a shared reverb)
```

**Case 2: Series Configuration (A → Reverb, B separate)**

```
Timeline:
A ──→ Spring Reverb ──→ Mixer ──→ Output
B ──────────────────┘

Freeze process:
1. Freeze Sample A output
   → A.frozenOutput[0..N] = A's samples
2. Freeze Reverb input (A's frozen output)
   → Reverb.freezePage(page, A.frozenOutput[0..N])
   → Reverb.frozenOutput[0..N] = reverb response to A only
3. Freeze Mixer (reverb + B)
   → Mixer.frozenOutput[0..N] = reverb + B
4. SCut A reads Reverb's frozen output ✓
   SCut B reads Mixer's frozen output ✓
```

### The Principle: One Frozen Output Per Wire

- Each component has **one frozen output page** for a given time range
- That frozen output is determined by its **frozen input pages** (from predecessor components)
- Multiple SCuts reading the same component all read the **same frozen pages**
- No ambiguity, no race conditions, no redundant computation

---

## State Invariants

### Invariant 1: Single Computation Per Wire per Time Range

For any component C and time range [T..T+pageSize):
- **Exactly one frozen page** represents C's output for [T..T+pageSize)
- All consumers read from **that same page**
- Computation happens once; the output is deterministic

### Invariant 2: Sequential Rendering from Reset

Freezing a component's page requires:
1. Reset to initial state (or restore state from previous page)
2. Feed input sequentially (page by page)
3. Capture output, store in page
4. Save internal state for next page's resumption

**No seeking, no jumping.** The sequential nature is intrinsic to freezing.

### Invariant 3: Determinism

```
Given:
  - Component C in state S_0 (reset or loaded from page)
  - Input signal I[0..N]
  - Sample rate R
Result:
  - Output O[0..N] is always the same (deterministic)
  - C's state after rendering: S_N (always the same)
```

This holds because:
- DSP math is deterministic (same inputs → same outputs)
- No random state changes during render
- Page snapshot captures exact internal state

### Invariant 4: No Concurrent Modification During Freeze

While a component's page is being frozen:
- Input pages are **read-only** (already frozen)
- Component's internal state is **private to the freeze operation**
- No other threads call seekTo(), calcOutputTo(), or modify parameters
- Freezing is an atomic operation (from reset/restore to page complete)

### Invariant 5: Page Boundaries Align

Pages for interconnected components align at time boundaries:
- Page size is consistent across all components (e.g., 256 kB = ~0.68s)
- Component A's page [0..P_size] feeds Component B's page [0..P_size]
- No off-by-one misalignment between pages

---

## Component Types and Freezing

### Type 1: Stateless/Memory-less Components

**Examples:** Oscillators, sample readers (with per-consumer reader state), mixers

**Freezing:**
- No internal state to save/restore
- Simply render sequentially, capture output
- Page is self-contained

```cpp
twOscillator::freezePage(offset_t startPos, ...) {
    if (startPos == 0) reset();
    // No state to restore (phase resets to 0)
    
    page->samples.resize(PAGE_SIZE);
    length_t got = renderFrames(page->samples.data(), PAGE_SIZE, nullptr);
    // No internal state to capture (phase is implicit in output)
    
    return page;
}
```

### Type 2: Sequential/Stateful Components

**Examples:** Spring Reverb, Delay, Convolution, any component with delay lines or feedback

**Freezing:**
- Capture internal state (delay line contents, feedback buffers, etc.) after render
- Restore state when freezing next page
- Pages are causally linked

```cpp
twSpringReverb::freezePage(offset_t startPos, const sample_t* inputData, ...) {
    if (startPos == 0) {
        reset();
    } else {
        auto prevPage = pages_[startPos - PAGE_SIZE];
        if (prevPage) {
            // Restore: load delay lines from previous page's snapshot
            delayLines_ = std::any_cast<DelayLineState>(prevPage->internalState);
        }
    }
    
    page->samples.resize(PAGE_SIZE);
    length_t got = renderFrames(page->samples.data(), PAGE_SIZE, inputData + startPos);
    
    // Snapshot: save delay line state for next page
    page->internalState = DelayLineState(delayLines_);
    
    return page;
}
```

**Causality:** Page [N..2N] depends on state at N, which is captured from page [0..N]. Pages form a causal chain.

---

## Page Cache Lifecycle

### Allocation (On-Demand)

```cpp
std::shared_ptr<OutputPage> twComponent::getOrAllocatePage(
    offset_t startPos,
    uint32_t aspects  // Preview/Playback/Export
) {
    std::lock_guard lock(pagesMutex_);
    
    auto it = pages_.find(startPos);
    if (it != pages_.end() && (it->second->validAspects & aspects)) {
        return it->second;  // Already frozen
    }
    
    // Allocate new page
    auto page = std::make_shared<OutputPage>();
    page->startPosition = startPos;
    pages_[startPos] = page;
    
    // Schedule async freezing
    revalidator->scheduleFreezing(this, startPos, aspects);
    
    // Return (not yet valid; consumer will retry or fallback)
    return page;
}
```

### Freezing (Background Worker)

```cpp
// In CaptureRevalidator worker thread:
void freezeComponentPage(twComponent* comp, offset_t startPos, uint32_t aspects) {
    // Get input pages (predecessors in component graph)
    auto inputPages = comp->getInputPages(startPos);
    
    if (!allPagesReady(inputPages)) {
        // Predecessors not yet frozen; retry later
        return;
    }
    
    // Freeze this component's page
    auto frozenPage = comp->freezePage(startPos, inputPages, aspects);
    frozenPage->validAspects = aspects;
    
    // Atomic swap into cache
    {
        std::lock_guard lock(comp->pagesMutex_);
        comp->pages_[startPos] = frozenPage;
    }
    
    // Signal downstream: this page is ready
    emit componentPageReady(comp, startPos, aspects);
}
```

### Release (Memory Management)

```cpp
// Playhead has moved far past; release old pages
void twComponent::releaseOldPages(offset_t keepAfterPos) {
    std::lock_guard lock(pagesMutex_);
    
    for (auto it = pages_.begin(); it != pages_.end(); ) {
        if (it->first + PAGE_SIZE < keepAfterPos - CACHE_RETENTION_WINDOW) {
            it = pages_.erase(it);  // Page out of use; deallocate
        } else {
            ++it;
        }
    }
}
```

---

## Data Flow: Three Sinks (Preview, Playback, Export)

### Path 1: Timeline Preview Paint

```
User paints timeline at position P:
1. For each visible cut C:
   a. Get C's frozen page at position P
   b. Extract preview peaks from page
   c. drawWaveform(peaks)

For container-backed cuts (groups, tracks):
1. Get container component's frozen page
2. If not ready: schedule freeze, show placeholder
3. Once frozen: draw preview peaks from frozen output
```

**Freezing Aspects:** Preview (waveform peaks only, not full audio)

### Path 2: Audio Playback

```
Playback loop (audio thread):
1. For each page interval [0..P_size], [P_size..2*P_size], ...
   a. Get root component's frozen page for this interval
   b. Read frozen samples to audio output buffer
   c. Write to device

No seekTo() calls. No position state. Just read frozen pages sequentially.
```

**Freezing Aspects:** Playback + Metadata

**Stale Data Fallback:** If page not yet frozen, read previous page or silence

### Path 3: File Export/Render

```
Export loop (render thread):
1. Create FileSink
2. For each page interval [0..P_size], [P_size..2*P_size], ...
   a. Get root component's frozen page (high priority)
   b. Read frozen samples
   c. FileSink buffers and writes to disk

Like playback, but with buffering for disk I/O efficiency.
```

**Freezing Aspects:** Export (full-quality audio)

---

## Handling Overlapping Indirect SCuts

### Scenario Redux: Clear Resolution

```
Two SCuts reference Sample X (via SLink):
├─ SCut A (project time 0:00–0:05, sample offset [0–10])
└─ SCut B (project time 0:02–0:07, sample offset [5–15])

Rendering project timeline:

Timeline 0:00–0:05:
  Sample X pages: [0–10] frozen
  SCut A reads Sample X.frozenPage[0–10]
  SCut B wants Sample X.frozenPage[5–15]
    → Need to freeze [5–15]
    → Start from reset, render 0–5 (waste? or cache it)
    → OR: smart freezing—freeze [5–15] with reset at offset 5? NO!
    → Actually: always freeze from 0. If B needs [5–15], freeze [0–15] in two pages.

Better approach: Freezing is always sequential from 0.
  → Page [0..P_size] frozen first (requires reset + sequential render)
  → Page [P_size..2*P_size] frozen next (restores state from page 1, continues render)
  → SCut A reads page 1 slice [0–10]
  → SCut B reads page 1 and 2 slices [5–15]
  → Both read same pages (no redundancy)
```

**Key:** Pages always start from position 0 and extend sequentially. Multiple SCuts reading different slices of the same pages all get correct, consistent results.

---

## Invalidation and Re-Freezing

### Trigger: Parameter Change

When a user modifies a component (e.g., reverb decay time):

```cpp
void twSpringReverb::setDecayTime(float newDecay) {
    decayTime_ = newDecay;
    
    // Invalidate all frozen pages (they were computed with old decay time)
    {
        std::lock_guard lock(pagesMutex_);
        for (auto& [pos, page] : pages_) {
            page->validAspects = 0;  // Mark as stale
        }
    }
    
    // Notify dependents: downstream components also need re-freezing
    notifyDependentsChanged();
    
    // Schedule re-freezing (async, low priority for now)
    revalidator->scheduleFreezing(this, 0, All);
}
```

### Propagation: Upstream Changes

If a child component changes, parents must also re-freeze:

```cpp
twComponent::notifyDependentsChanged() {
    // Invalidate parent components' pages
    for (twComponent* parent : parents_) {
        parent->invalidateAllPages();
        parent->notifyDependentsChanged();  // Propagate up
    }
}
```

### Re-freezing Priority

- **High:** Playback + Metadata (audio must not drop)
- **Medium:** Export (user-initiated, not real-time)
- **Low:** Preview (UI can show stale)

---

## Memory Model and Thread Safety

### Page Access (Read)

```cpp
// Consumers read frozen pages
auto page = component->getOrAllocatePage(pos, PLAYBACK);
if (page && (page->validAspects & PLAYBACK)) {
    // Read-only access; safe from multiple threads
    copyToOutput(page->samples, output);
}
```

**Thread Safety:** Pages are immutable once frozen. Multiple readers are safe. Refcount prevents deallocation while readers hold shared_ptr.

### Page Freezing (Write)

```cpp
// Freezing happens on revalidator thread only
// One freezing operation per component per time range
std::lock_guard lock(comp->pagesMutex_);
comp->pages_[startPos] = frozenPage;  // Atomic swap
```

**Thread Safety:** Mutex protects pages_ map. Freezing is serial (one worker per page). No concurrent freezing of the same page.

### Component Parameters (Read During Freeze)

```cpp
twSpringReverb::freezePage(...) {
    // Read decayTime_ (may change on UI thread)
    float decay = decayTime_;  // Atomic or snapshot?
    
    // Problem: UI thread changes decayTime_ mid-freeze
    // Solution: Snapshot parameters before freezing starts
    auto snapshot = getParameterSnapshot();
    float decay = snapshot.decayTime;  // Use snapshot for entire freeze
}
```

**Thread Safety:** Parameters are snapshotted before freezing. UI thread changes update parameters; revalidator sees next freeze with new values.

---

## Success Criteria

- ✅ Sequential/stateful components (reverbs, delays) freeze correctly with internal state snapshots
- ✅ Multiple indirect SCuts referencing same component read same frozen pages (no redundancy, no race)
- ✅ No seekTo() called during playback/export (all reads are from frozen pages)
- ✅ Page cache memory is bounded (old pages released when no longer needed)
- ✅ Stale data fallback prevents audio dropouts while freezing
- ✅ Parameter changes invalidate and re-freeze pages (correct audio)
- ✅ Determinism: same input + same component state → same output (always)
- ✅ Performance ≥ Phase 4 (freezing should be no slower than current rendering)
- ✅ No deadlocks or race conditions (clear mutex/refcount discipline)

---

## Implementation Roadmap

### Phase A: Component Base Class Changes
- [ ] Add OutputPage struct (samples, state snapshot, validAspects)
- [ ] Add pages_ cache to twComponent
- [ ] Implement getOrAllocatePage(), releaseOldPages()
- [ ] Add reset() virtual method (if not already present)
- [ ] Add internalState capture/restore mechanism (std::any or virtual methods)

### Phase B: Sequential Rendering Loop
- [ ] Create freezePage() virtual method in twComponent
- [ ] Implement in stateless components (oscillators, mixers)
- [ ] Implement in stateful components (reverb, delay, grain)
- [ ] Test: verify output matches current calcOutputTo() for stateless components

### Phase C: Revalidator Integration
- [ ] Extend CaptureRevalidator to schedule component page freezing
- [ ] Implement prioritized freezing (Playback > Export > Preview)
- [ ] Handle cascade: freeze children before freezing parents
- [ ] Implement invalidation propagation

### Phase D: Render Path Refactoring
- [ ] Replace PlaybackSink: read from root component's frozen pages (no seekTo)
- [ ] Replace FileSink: read from root component's frozen pages (no seekTo)
- [ ] Replace PreviewSink: read from root component's frozen pages (extract peaks)
- [ ] Remove direct seekTo()/calcOutputTo() calls from render loops

### Phase E: Testing & Validation
- [ ] Unit tests: freezePage correctness for each component type
- [ ] Integration tests: overlapping indirect SCuts produce correct output
- [ ] Regression tests: render output identical to Phase 4
- [ ] Performance benchmarks: compare freezing vs. current paths

---

## Comparison: V1 → V2

| Aspect | V1 | V2 |
|--------|----|----|
| **Model** | Page cache at SCut level; components pulled on-demand | Page cache at component output level; all consumers read frozen |
| **Sequential Components** | Broken (can't jump to arbitrary time) | Works naturally (freeze sequentially from reset) |
| **Multiple SCuts, Same Component** | Ambiguous; potential race on component state | Clear (all read same frozen pages) |
| **Seekto() Calls** | Many (per-render, per-component) | None (only in freezing, which is atomic) |
| **State Snapshots** | Only for SCut parameters | Full internal state (delay lines, feedback, etc.) |
| **Redundant Computation** | Possible if two SCuts pull same component | Eliminated (freeze once, all read) |
| **Memory Model** | Triple-buffered SCut pages | Double-buffered component output pages + refcounting |
| **Thread Safety** | Complex (snapshot + try_lock + stale fallback) | Simpler (freezing is atomic, reading is R/O) |

---

## Open Questions for Implementation

1. **Internal State Format:** Use `std::any` (type-unsafe, flexible) or virtual methods (type-safe, boilerplate)?
2. **State Restoration:** How to serialize/deserialize component state between freeze calls?
3. **Preview Aspect:** Extract peaks from frozen samples, or freeze as separate aspect?
4. **Page Size:** Fixed 256 kB, or adaptive based on component complexity?
5. **Fallback Strategy:** On stale page, use previous page or silence?
6. **Concurrency:** Can multiple freezing operations happen in parallel (on different components), or serial only?

---

## Related Concepts

- **DAW Track Freezing:** Same idea—pre-render track DSP, read cached audio
- **GPU Caching:** Analogous to render target/FBO caching in graphics
- **Functional Programming:** Immutable data (frozen pages) + pure functions (component rendering)

