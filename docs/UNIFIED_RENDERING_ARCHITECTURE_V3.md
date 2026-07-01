# Unified Rendering Architecture V3

## Overview

Smaragd's rendering pipeline unifies real-time audio playback, preview waveform generation, and export functionality through a single **page-based freezing mechanism** with explicit **state snapshots** for continuous audio processing.

This document describes the V3 architecture after Phase 1–5 refactoring and the introduction of the `twView` wrapper for stable clip references.

---

## Architecture Layers

### Layer 1: Platform Audio Backend (OS-specific)
- **WASAPI** (Windows), **ALSA** (Linux), **CoreAudio** (macOS)
- Implement `AudioBackend` interface
- Pull audio samples via callback at device buffer rate
- Device sample rate may differ from project rate

### Layer 2: Audio Engine (`tw303a/src/audio/audio_engine.cc`)
- **Responsibility:** Coordinate resampling and page-based rendering
- **Page Management:**
  - Tracks current position (`currentPos_`)
  - Maintains frozen pages in cache
  - Handles page transitions and state resumption
- **Resampling:** Configurable input/output rates
  - Engine renders at project rate (48 kHz default)
  - AudioEngine resamples to device rate via linear interpolation
- **Non-blocking:** Uses previously-frozen pages if new page not ready

### Layer 3: twSpeaker (`tw303a/src/twspeaker.cc`)
- **Bridge:** Connects AudioBackend to DSP engine
- Hosts twComponent (synth) and twResampler
- Lambda callback pulls frames from AudioEngine
- Applies format conversion (mono → stereo, float → int16/int32)

### Layer 4: DSP Component Tree (`tw303a/`)
- **Root:** `twRewire` — channel router
  - Owns `twTrackMix[]` — one per audio bus
  - Composes audio from all tracks
- **Per-Track:** `twTrackMix` — clip mixer
  - Owns `twView[]` wrappers (one per timeline clip)
  - Iterates clips and mixes their frozen output
- **Per-Clip:** `twView` — stable component wrapper
  - Forwards calls to actual component (obtained via callback)
  - Enables dynamic component lookup without stale pointers
- **Leaf Components:** Readers, filters, oscillators, grain sources
  - Implement `freezePage()` to render audio to page buffers
  - Restore state from `previousPage` to maintain continuity

---

## Page-Based Freezing Pipeline

### Concept: "Freeze" = Deterministic Snapshot

A **frozen page** is a contiguous buffer of audio samples at a specific timeline position, plus a state snapshot for resuming at the next page boundary.

**Page Structure:**
```cpp
struct twOutputPage {
    uint64_t startPosition;                    // Timeline position (samples)
    uint32_t validFrames;                      // Number of valid samples
    std::vector<float> samples;                // 65,536 frames (256 KB)
    std::array<uint8_t, 1024> internalState;  // Component state snapshot
    uint32_t generation;                       // Invalidation marker
};
```

### Signal Flow: CoreAudio → Page → Device

```
CoreAudio Callback (device rate)
    ↓
twspeaker.cc lambda (pulls audio)
    ↓
AudioEngine::pullBlock()
    ├─ if (resampling needed):
    │    pull inFramesNeeded at project rate
    │    → pullStereoFrameFrozen() (65536 frames or fewer)
    │    → resample to device rate
    └─ else: passthrough
    ↓
pullStereoFrameFrozen()
    ├─ load currentPos (position in timeline, atomic)
    ├─ updateFrozenPage(currentPos)
    │    └─ request page from synth at page boundary
    │       (e.g., page [0, 65536), [65536, 131072), etc.)
    ├─ extract sample[pageFrameOffset] from currentFrozenPage
    ├─ advance pageFrameOffset, currentPos
    └─ return sample to caller
    ↓
Device Buffer (44.1 kHz, 16-bit, stereo, etc.)
```

### Page Rendering: freezePage()

**Single call:** `component->freezePage(startPos, nullptr, 0, FRAME_CAPACITY, sampleRate, previousPage)`

**Execution (twComponent::freezePage):**
1. **Lock & Cache Check** (under mutex):
   - Is page at `startPos` already in `outputPages_` cache?
   - If yes, return cached page immediately
   - If no, allocate placeholder, mark `needsRendering = true`
2. **Release Lock** (critical: avoid deadlock on recursive freezePage calls)
3. **Render** (outside lock, inside FreezeContext):
   - Install `FreezeContext` guard for this rendering phase
   - Pre-freeze all upstream input components
   - If `previousPage` provided:
     - Restore component internal state from `previousPage->internalState`
   - Else:
     - Call `reset()` (first page only)
   - Call `renderFrames()` to populate sample buffer
   - Capture new internal state snapshot
4. **Mark Valid** (re-acquire lock):
   - Set `validAspects = twAspectAll`
   - Release lock
5. **Return page**

### FreezeContext: Explicit Rendering Phase Management

**Problem:** Preventing infinite recursion during frozen rendering requires distinguishing between:
- **Normal streaming:** `calcOutputTo` pulls data dynamically via `freezePage` on upstream components
- **Frozen rendering:** Components must use pre-computed input, not call `freezePage` recursively

**Old Approach:** Thread-local boolean flag `g_inCalcOutputToPath` scattered across codebase, easy to forget in new code paths.

**New Approach (Phase 6):** Explicit `FreezeContext` RAII guard.

**Architecture:**
```cpp
// In freezePage_nolock:
FreezeContext freezeCtx(*this);  // Install rendering context

// Pre-freeze all upstream inputs and register them
for (idx_t i = 0; i < getNInputs(); i++) {
    if (auto plug = getInputPlug(i)) {
        auto upstream = &plug->getParentLatch().getComponent();
        auto page = upstream->freezePage(...);  // Recursively freeze dependencies
        freezeCtx.setInputPage(i, page);        // Store pre-frozen input
    }
}

// Now renderFrames() and calcOutputTo() can access pre-frozen data
renderFrames(...);
// FreezeContext auto-destructs, restoring previous context
```

**Benefits:**
- **Explicit:** `FreezeContext ctx(*this)` makes rendering phase obvious
- **Type-safe:** C++ RAII guard, not a magic boolean
- **Extensible:** Can hold metadata (sample rates, flags, etc.) without adding more thread-locals
- **Architectural:** Pre-freezing eliminates recursion at the source instead of working around it
- **Safe:** RAII ensures cleanup and proper nesting of contexts

**Query (in copyData/readStreamingData):**
```cpp
FreezeContext* ctx = FreezeContext::current();
if (ctx) {
    // Use pre-frozen input page (no recursion risk)
    auto page = ctx->getInputPage(myInputIndex);
    if (page) { /* copy from page */ return; }
}
// Fallback to normal streaming path
```

### State Continuity Across Pages

**Problem:** Without state snapshots, components would reset at every page boundary, causing stuttering or looping.

**Solution:** Each component captures its internal state after rendering, and restores it before rendering the next page.

**Flow:**
```
Page 0: freezePage(0, ..., nullptr)
  ├─ reset() called (first page)
  ├─ renderFrames(samples[0..65535])
  ├─ component modifies position: reader.pos = 65536
  ├─ captureInternalState() → page0.internalState
  └─ return page0

Page 1: freezePage(65536, ..., page0)
  ├─ restoreInternalState(page0.internalState) → reader.pos = 65536
  ├─ renderFrames(samples[65536..131071])
  ├─ reader continues from where page 0 left off
  ├─ component now at: reader.pos = 131072
  ├─ captureInternalState() → page1.internalState
  └─ return page1

Result: Seamless audio with no resets
```

---

## Clip Management: twTrackMix → twView Chain

### Timeline Representation

**Old (Phase 1 Input):** `twTrackMix` read `STrack::childLinks()` directly on every audio buffer, creating:
- Boundary violation (tw303a reaches into SObject hierarchy)
- Tight coupling
- Potential race conditions (UI modifying list while audio reads)

**New (Phase 1–5):** `twTrackMix` owns clip list, STrack notifies via push model.

### Clip Lifecycle

**1. Add Clip (STrack → twTrackMix)**
```cpp
// UI thread: user adds SCut to track at timeline position 5000
STrack::trackChildWasAdded(SLink &child) {
    auto getComponentFn = [&child]() { return &child.getRootComponent(); };
    cpTrackMixers_[i]->insertClip(5000, duration, getComponentFn);
}

// twTrackMix::insertClip():
twView *view = new twView(env, getComponentFn);  // stable wrapper
view->init();
clips_.push_back({5000, duration, view, nullptr}); // nullptr = first page
```

**2. Render Clip (Audio Thread)**
```cpp
// twTrackMix::freezePage_nolock():
for (ClipEntry &clip : clips_) {
    uint64_t childPos = pageStartPos - clip.startTime;  // relative offset
    
    // Call getComponentFn dynamically (current reader, not stale)
    auto childPage = clip.view->freezePage(
        childPos,
        nullptr,
        0,
        FRAME_CAPACITY,
        sampleRate,
        clip.previousPage  // resume from previous page's state
    );
    
    clip.previousPage = childPage;  // save for next page
    
    // Mix into track output
    for (uint32_t i = 0; i < childPage->validFrames; ++i)
        page->samples[destOffset + i] += childPage->samples[i];
}
```

**3. Remove Clip (STrack → twTrackMix)**
```cpp
// UI thread: user deletes clip from track
STrack::trackChildWasRemoved(SLink &child) {
    auto getComponentFn = [&child]() { return &child.getRootComponent(); };
    cpTrackMixers_[i]->removeClip(getComponentFn);
}

// twTrackMix::removeClip():
for (auto it = clips_.begin(); it != clips_.end(); ++it) {
    if (/* clip matches */) {
        delete it->view;  // destroy wrapper
        clips_.erase(it);
    }
}
```

### twView: Stable Wrapper, Dynamic Lookup

**Why twView?**
- `SCut::getRootComponent()` can return different readers as state changes
- Storing raw `twComponent*` leads to stale pointers
- Solution: wrapper holds callback, invokes it each time

**Architecture:**
```cpp
class twView : public twComponent {
    std::function<twComponent*()> getComponentFn_;
    
    virtual int seekTo(offset_t pos) override {
        twComponent *comp = getComponentFn_();  // dynamic lookup
        return comp->seekTo(pos);
    }
    
    virtual std::shared_ptr<twOutputPage> freezePage(...) override {
        twComponent *comp = getComponentFn_();
        return comp->freezePage(...);
    }
};
```

**Result:**
- twTrackMix stores stable `twView*` (safe to dereference)
- twView forwards to current component (via callback)
- SObject changes handled transparently
- No stale pointers, no crashes on seek

---

## Decoupling: tw303a ↔ main/

### Boundary

```
┌─────────────────────────────────┐
│ tw303a (DSP Library)            │
├─────────────────────────────────┤
│ twComponent, twTrackMix, twView │
│ No SObject, SLink, STrack       │
│ No Qt dependencies              │
└─────────────────────────────────┘
              ↑
         std::function
      callbacks (generic)
              ↓
┌─────────────────────────────────┐
│ main/ (UI + Model)              │
├─────────────────────────────────┤
│ SObject, STrack, SLink, SCut    │
│ Qt widgets & signals            │
└─────────────────────────────────┘
```

### Synchronization Pattern

**UI Thread (STrack):**
```cpp
// Captures SLink by reference; called once per clip add/move/remove
auto getComponentFn = [&slink]() { return &slink->getRootComponent(); };
trackMixer->insertClip(startTime, duration, getComponentFn);
```

**Audio Thread (twTrackMix):**
```cpp
// Invokes callback on every page render
twComponent *comp = clip.view's callback();  // returns current component
page = comp->freezePage(...);
```

**Result:**
- tw303a is stateless regarding SObject
- STrack owns all timeline semantics
- Callback bridges are invoked atomically (fast)
- No locks needed between threads (callback is const operation)

---

## Preview Rendering

### freezePreviewPage()

Similar to `freezePage()`, but at lower resolution for waveform display.

**Purpose:** Draw peaks in timeline editor without full rendering cost.

**Typical Params:**
```cpp
component->freezePreviewPage(
    startPos,
    length,             // often 2048 samples (lower res)
    previewSampleRate,  // 1 kHz
    fullSampleRate,     // 48 kHz
    previousPage        // state snapshot
);
```

**Integration:**
- `CaptureRevalidator` schedules preview jobs for lazy invalidation
- `SCut` and other objects call `freezePreviewPage()` via revalidator
- Pages cached per aspect (Preview, Playback, Metadata, Export)

---

## Export Rendering

**Flow:**
1. User selects `File → Render...`
2. Choose format (WAV, OGG, MP3), quality
3. UI creates `RenderSession` (background thread)
4. RenderSession:
   - Calls `AudioEngine::pullBlock()` at project rate
   - Pulls frames into buffer
   - Writes via `AudioFileWriter` (WAV, OGG, MP3 variants)
   - Emits progress callbacks
5. UI shows progress dialog
6. Exported file saved to disk

**Decoupling:** Export uses same `freezePage()` path as playback; rendering is unified.

---

## Critical Invariants

1. **State Snapshots Flow Forward:**
   - Page N+1 always resumes from Page N's snapshot
   - Maintains reader position, filter state, oscillator phase, etc.

2. **Clips Owned by twTrackMix:**
   - STrack notifies via callbacks
   - Audio thread never reads SObject directly
   - No concurrent modification of clips list (mutex protected)

3. **Page Cache Prevents Redundant Rendering:**
   - Same `startPos` returns cached page immediately
   - Invalidation only on generation mismatch

4. **twView Stable for Clip Lifetime:**
   - Created when clip added
   - Destroyed when clip removed
   - Safe to store `twView*` in ClipEntry

5. **Non-blocking Playback:**
   - Page not ready? Use previous page
   - Never waits for revalidation
   - Audio keeps flowing, UI refreshes later

6. **FreezeContext Enables Safe Rendering:**
   - Active only during `freezePage_nolock()` execution
   - All upstream dependencies pre-frozen before rendering starts
   - Prevents infinite recursion: `copyData` accesses pre-frozen pages, not live `freezePage` calls
   - Automatic cleanup via RAII guard ensures no cross-render contamination
   - Thread-local storage allows nesting of contexts for hierarchical components

---

## Typical Scenario: Playing a Multi-Clip Track

### Timeline Setup
```
Track 1 (48 kHz project)
├─ SCut A: samples [0, 10000), timeline [1000, 11000)
├─ SCut B: samples [0, 20000), timeline [20000, 40000)
└─ SPlainWave: timeline [50000, 55000)

Device: CoreAudio @ 44.1 kHz
```

### Playback @ Position 25000

**twspeaker (every ~1024 samples from device):**
```
pullBlock(outL, outR, nFrames=1024)
  ├─ configureResampling(48000, 44100)
  ├─ pullStereoFrameFrozen() × ~1111 times (48000/44100 ratio)
  ├─ resample 1111 frames → 1024 samples @ 44.1 kHz
  └─ copy to outL, outR
```

**AudioEngine::pullStereoFrameFrozen():**
```
pos = 25000
updateFrozenPage(25000)
  ├─ pageStartPos = (25000 / 65536) * 65536 = 0
  ├─ currentFrozenPage already at page 0?
  │   No (first page request) → freeze it
  │   freezeRequest(0, ..., nullptr)
  └─ page0 cached
```

**twRewire::freezePage(0, FRAME_CAPACITY, 48000, nullptr):**
```
→ twTrackMix::freezePage(0, ...)
   ├─ reset() called (first page)
   ├─ iterate clips:
   │   ├─ SCut A: overlaps [0, 65536)? 
   │   │    YES (timeline [1000, 11000))
   │   │    childPos = max(0, 0 - 1000) = 0 (clip not started yet)
   │   │    A->freezePage(0, ..., nullptr)
   │   │      └─ render 65536 frames, starting at sample 0
   │   │      └─ capture state
   │   ├─ SCut B: overlaps [0, 65536)? 
   │   │    NO (timeline [20000, 40000)) → skip
   │   └─ SPlainWave: overlaps? NO (timeline [50000, 55000)) → skip
   ├─ mix A's output at destOffset=1000
   ├─ apply track gain/mute
   └─ return page0
```

**Back to pullStereoFrameFrozen():**
```
pageFrameOffset = 25000 - 0 = 25000
sample = page0->samples[25000]  (position within clip A)
pageFrameOffset++, pos++
currentPos_ = 25001
```

**Repeat ~1111 times** (for ~1024 output samples @ 44.1 kHz):
```
pos increases: 25000 → 25001 → ... → 26111
Still in page 0 (which covers [0, 65536))
Read from page0->samples[25001], page0->samples[25002], etc.
```

### Page Boundary @ Position 65536

**Next pullStereoFrameFrozen() call:**
```
pos = 65536
updateFrozenPage(65536)
  ├─ pageStartPos = (65536 / 65536) * 65536 = 65536 (new page)
  ├─ currentFrozenPage at page 0 (generation check passes)
  ├─ Page 0 ≠ 65536 → need new page
  ├─ freezeRequest(65536, ..., page0)  ← pass page 0 as state snapshot
  └─ page1 cached
```

**twTrackMix::freezePage(65536, ..., page0):**
```
├─ iterate clips:
│   ├─ SCut A: overlaps [65536, 131072)?
│   │    NO (timeline [1000, 11000)) → skip
│   ├─ SCut B: overlaps? 
│   │    YES (timeline [20000, 40000)) → ACTIVE NOW
│   │    childPos = 65536 - 20000 = 45536
│   │    B->freezePage(45536, ..., clip_B.previousPage)
│   │      └─ B's previousPage is nullptr (first time B renders)
│   │      └─ reset() called for B
│   │      └─ render frames [45536, 45536+65536) = [45536, 111072)
│   │      └─ but B only has samples [0, 20000), so produces silence after
│   │      └─ capture state
│   │    clip_B.previousPage = page of B
│   └─ ...
├─ mix A's output (if still active) + B's output
└─ return page1
```

### Seeking @ Timeline Position 30000

**User clicks @ 30000; SMainWindow::startPlaying() calls SStdMixer::seekTo(30000)**

**SStdMixer::seekTo(30000):**
```
→ STrack::seekTo(30000)
→ twTrackMix::seekTo(30000)  [acquires mutex]
→ twTrackMix::seekTo_nolock(30000)
   ├─ for each clip:
   │   ├─ SCut A: clipRelative = max(0, 30000 - 1000) = 29000
   │   │   A->seekTo(29000)  [reader seeks within SCut's samples]
   │   ├─ SCut B: clipRelative = max(0, 30000 - 20000) = 10000
   │   │   B->seekTo(10000)
   │   └─ SPlainWave: not active yet
   ├─ release mutex
   ├─ currentPos_ = 30000 (atomic store)
   └─ AudioEngine will invalidate cached page on next render
```

**Next audio pull:**
```
pos = 30000
updateFrozenPage(30000)
  ├─ pageStartPos = 0 (same as before, page 0)
  ├─ currentFrozenPage is page 0, but generation has changed
  │   (invalidated by seek) → drop reference
  ├─ freezeRequest(0, ..., nullptr)  ← restart from beginning
  └─ new page 0 rendered with readers at their seek positions
```

---

## Performance Considerations

### Page Caching
- 65,536-frame page @ 48 kHz = ~1.4 seconds
- Typical buffer callback: 1024–4096 samples = few pages/sec needed
- Cache hits dominant; rendering rare

### State Snapshots
- Stored in page (1024 bytes)
- Captured after each page render
- Restored before next page (cost: memcpy only)
- Enables seamless continuity

### Resampling
- Linear interpolation, fast (in-place)
- Only when project rate ≠ device rate
- Configurable at runtime

### Threading
- Audio thread: reads frozen pages, non-blocking
- UI thread: modifies clips via callbacks, no locks held
- Revalidator: background jobs, non-blocking

---

## Future Enhancements

1. **IIR Resampling:** Replace linear with higher-quality (SincInterpolator)
2. **Parallel Page Rendering:** Pre-render next page while playing current
3. **Disk Streaming:** Stream large samples instead of buffering entirely
4. **Plugin State:** Extend snapshots to include plugin internal state
5. **SIMD Optimization:** Vectorize mixing, resampling, format conversion

---

## Summary

The V3 architecture unifies real-time playback, preview, and export via:
- **Page-based freezing** with explicit state snapshots
- **FreezeContext** for safe, explicit rendering phases (Phase 6)
  - Pre-freezes all upstream dependencies before rendering
  - Eliminates recursion at the architectural level
  - RAII guard ensures proper cleanup and nesting
- **twView wrappers** for stable clip references with dynamic component lookup
- **Push-model clip management** (STrack → twTrackMix via callbacks)
- **Strict decoupling** between tw303a (DSP) and main/ (UI)
- **Non-blocking audio** that tolerates preview lag
- **Lock-free coordination** via atomics and snapshots

This design enables **seamless audio playback** with **continuous state**, **no stale pointers**, **clear separation of concerns**, and **safe extensibility through explicit rendering contexts**.
