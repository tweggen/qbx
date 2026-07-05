# Smaragd Audio Signal Chain

## Overview

Smaragd's audio architecture separates the **UI model** (SObject hierarchy) from the **DSP engine** (twComponent graph). This document traces how audio flows from project structure through DSP components to hardware output.

## SObject Model Hierarchy

The UI layer organizes audio content as a tree:

```
SProject
  └── SStdMixer (root SObject, set via SProject::setRootComponent())
        ├── SLink → STrack (Track 0)
        │     ├── SLink → SCut → SLink → SPlainWave (WAV clip)
        │     ├── SLink → SCut → SLink → SPlainWave (another clip)
        │     └── SPluginChain (effect inserts)
        │
        └── SLink → STrack (Track 1, N tracks total)
              ├── SLink → SCut → SLink → SPlainWave
              └── SPluginChain
```

**Key objects:**
- **SProject:** Project container; owns root component via `setRootComponent()`
- **SStdMixer:** Root mixer SObject; typically set as project root
- **STrack:** Per-track container; owns clips and plugin chain
- **SCut:** Timeline clip; wraps a media source (SPlainWave, etc.)
- **SPlainWave:** WAV file wrapper; provides `getRootComponent()` → `twWavInput`
- **SPluginChain:** Effect chain container (currently a placeholder)

## twComponent DSP Graph

The DSP engine creates a parallel graph from the SObject tree. Each SObject has a DSP "root component" (queried via `getRootComponent()`) that performs synthesis/rendering.

### Signal Flow Diagram

```
┌─────────────────────────────────────────────────────────────┐
│ [AudioEngine + Read-Ahead Thread]                           │
│  └─ freezePage() on synthOutput_ (root mixer)               │
└────────────────────────────┬────────────────────────────────┘
                             │
              ┌──────────────▼──────────────┐
              │  SStdMixer's twRewire       │  (SStdMixer::getRootComponent())
              │  (master output router)    │
              └──────────────┬──────────────┘
                             │
         ┌───────────────────┼───────────────────┐
         │                   │                   │
    input[0]            input[1]              ...
         │                   │
         ▼                   ▼
    ┌─────────┐          ┌─────────┐
    │ twMixer │          │ twMixer │  (per-bus master mixer)
    │ [bus 0] │          │ [bus 1] │
    └────┬────┘          └────┬────┘
         │                   │
    input[0]             input[0]
         │                   │
         ▼                   ▼
    ┌──────────────┐     ┌──────────────┐
    │ STrack[0]    │     │ STrack[1]    │  (per-track mixer)
    │ ::twRewire   │     │ ::twRewire   │  (STrack::getRootComponent())
    └──────┬───────┘     └──────┬───────┘
           │                    │
           └────────────────┬───┘
                            │
                       input[0]
                            │
                            ▼
                    ┌───────────────────┐
                    │ twPluginChain[0]  │  (effect insert chain)
                    │ (per-track sends) │
                    └───────────┬───────┘
                                │
                           input[0]
                                │
                                ▼
                    ┌───────────────────┐
                    │ twTrackMix[0]     │  (timeline mixer: clips + state)
                    │ (STrack's clips)  │
                    └───────────┬───────┘
                                │
          ┌─────────────────────┼─────────────────────┐
          │                     │                     │
       clip[0]               clip[1]              clip[n]
          │                     │                     │
          ▼                     ▼                     ▼
      ┌─────────┐           ┌─────────┐         ┌─────────┐
      │ twView  │           │ twView  │         │ twView  │  (stable wrapper)
      │ [seek]  │           │ [seek]  │         │ [seek]  │  (wraps dynamic component)
      └────┬────┘           └────┬────┘         └────┬────┘
           │                     │                   │
        getComponent()        getComponent()    getComponent()
           │                     │                   │
           ▼                     ▼                   ▼
    ┌──────────────┐     ┌──────────────┐   ┌──────────────┐
    │ SCut[0]'s    │     │ SCut[1]'s    │   │ SCut[n]'s    │
    │ component    │     │ component    │   │ component    │
    │(twSampleRd) │     │(twSampleRd) │   │(twSampleRd) │
    └──────┬───────┘     └──────┬───────┘   └──────┬───────┘
           │                     │                   │
           ▼                     ▼                   ▼
    ┌──────────────┐     ┌──────────────┐   ┌──────────────┐
    │ twWavInput   │     │ twWavInput   │   │ twWavInput   │
    │ [SPlainWave] │     │ [SPlainWave] │   │ [SPlainWave] │
    │ (file reads) │     │ (file reads) │   │ (file reads) │
    └──────────────┘     └──────────────┘   └──────────────┘
                               │
                    (mixed at timeline positions)
                               │
              ┌────────────────▼──────────────┐
              │       twSpeaker               │
              │  (output sink + resampler)    │
              │  2 inputs (L/R)               │
              │  → Format conversion          │
              │  → CoreAudio/WASAPI/ALSA      │
              └────────────────┬──────────────┘
                               │
                               ▼
                   [Audio Hardware Device]
```

### Component Reference

| Component | Role | Stateful? | Notes |
|-----------|------|-----------|-------|
| **twRewire** | Router: 1→1 input passthrough | No | Connects track mixers to master; reads one input plug |
| **twMixer** | Mixer: N→1 inputs combined | No | Reads N input plugs, mixes with per-channel volume |
| **twPluginChain** | Effect insert container | Varies | Chains plugins in series (currently placeholder) |
| **twTrackMix** | Timeline mixer: clips + level | Yes | Stores clips with timeline info; applies track gain/mute |
| **twView** | Stable wrapper | No | Forwards calls to dynamically-resolved component via callback |
| **twSampleReader** | Per-clip playback cursor | Yes | Seekable; maintains position in clip relative to track start |
| **twWavInput** | File reader | No | Reads samples from WAV file at current position |
| **twSpeaker** | Terminal sink | Varies | Resampler + format converter; pulls from DSP root |

## Component Rendering Modes

### Page-Based Rendering (freezePage)

Modern rendering uses page-based snapshots for non-blocking playback:

1. **Read-ahead thread** calls `root->freezePage(startPos, ...)` to render N frames ahead
2. Each component either:
   - Implements custom `freezePage()` (e.g., `twTrackMix` iterates clips)
   - Inherits base implementation (e.g., `twRewire`, `twMixer` use `calcOutputTo`)
3. **Audio callback** calls `getPageIfExists(pos)` → retrieves pre-rendered page (fast path, no locks)
4. If page not ready → returns silence (graceful degradation)

**Participation:**

| Component | Method | Behavior |
|-----------|--------|----------|
| twTrackMix | Override | Snapshots clips, freezes each, mixes at timeline positions |
| twView | Override | Forwards to underlying component (via callback) |
| twRewire | Base class | Calls `calcOutputTo()` → `readStreamingData()` → upstream freezePage |
| twMixer | Base class | Calls `calcOutputTo()` × N inputs → upstream freezePage recursively |
| twPluginChain | Base class | Same (chain processing via calcOutputTo) |
| twSampleReader | Base class | `captureInternalState()` saves position; `restoreInternalState()` resumes |
| twWavInput | Base class | `calcOutputTo()` reads WAV at current position |

### Streaming Rendering (calcOutputTo)

Fallback for real-time synthesis or when pages unavailable:

1. **Lock-free audio thread** checks: is component ZOMBIE? → return silence
2. Component reads inputs, produces output synchronously
3. Used during:
   - Rendering freezePage frames (read-ahead thread)
   - Real-time synthesis if page miss occurs
   - UI preview rendering

## Teardown Protocol: Safe Component Removal

When a track or clip is deleted during playback, the teardown protocol ensures safe, non-blocking removal:

### Three-Phase Protocol

```cpp
// Phase 1: Mark ZOMBIE (atomic, ~1ns)
state_.store(ComponentState::ZOMBIE, std::memory_order_release);

// Phase 2: Deregister from parent (brief lock, ~10µs)
if (auto parent = parentComponent_.lock()) {
    parent->removeInput(myInputIndex_);  // Parent nulls its reference
}

// Phase 3: Notify dependents (snapshot + callback)
std::vector<twComponent*> depsCopy;
{
    std::lock_guard<std::mutex> lock(mutex());
    depsCopy = dependents_;  // Snapshot while holding lock
}
// Release lock → notify without holding it
for (auto dep : depsCopy) {
    if (dep) dep->onDependencyTeardown(this);
}

// Recursive teardown (no locks during recursion)
for (auto child : childrenSnapshot) {
    child->teardown();  // Child deregisters from this component
}
```

### Audio Thread Impact

```cpp
// Audio thread in calcOutputTo:
if (state_.load(std::memory_order_acquire) == ComponentState::ZOMBIE) {
    return dest.fillSilence(0, dest.length());  // ~1 CPU cycle overhead
}
```

- **Cost:** 1 atomic load with acquire semantics (typically 1–2 CPU cycles)
- **Latency:** 0 — check happens before lock acquisition
- **Behavior:** Removed components output silence immediately; playback continues uninterrupted

### Parent Tracking

Each component stores:
- `std::weak_ptr<twComponent> parentComponent_` — prevents circular refs
- `idx_t myInputIndex_` — index in parent's input array

When child removes from parent:
```cpp
parent->removeInput(myInputIndex_);  // Parent: pInputPlugs_[idx] = nullptr
```

No forced ejection; clean bidirectional handoff.

### Example: Removing a Track During Playback

1. **UI:** User clicks delete on STrack
2. **Teardown cascade:**
   - STrack's twRewire → marks ZOMBIE, deregisters from master rewire
   - twPluginChain → marks ZOMBIE, deregisters from track rewire
   - twTrackMix → marks ZOMBIE, snapshots clips, tears down each clip
   - Each clip's twView → marks ZOMBIE, tears down underlying twSampleReader
   - twSampleReader → marks ZOMBIE, deregisters from track mix
3. **Audio thread:** During next `calcOutputTo()`, detects ZOMBIE → outputs silence
4. **Result:** Track fades to silence; no hanging locks, no dangling pointers

## Wiring and Initialization

### Key Initialization Sites

| Step | File | Function | Lines |
|------|------|----------|-------|
| Create SStdMixer as project root | `main/src/smainwindow.cpp` | `fileNew()` | ~217 |
| Connect root to speaker (twSpeaker) | `main/src/sapplication.cpp` | `rewireSpeaker()` | 51–63 |
| Create twMixer + twRewire for master | `main/src/sstdmixer.cpp` | `setNBusses()` | 219–286 |
| Wire tracks into mixer inputs | `main/src/sstdmixer.cpp` | `reconnectTracksToMixer()` | 134–169 |
| Create per-track components (mix/chain/rewire) | `main/src/strack.cpp` | `setNBusses()` | 221–305 |
| Wire twTrackMix → twPluginChain → twRewire | `main/src/strack.cpp` | (in setNBusses) | 282–288 |
| Add clip to timeline | `main/src/strack.cpp` | `trackChildWasAdded()` | 175–200 |
| Wrap clip in twView; insert into twTrackMix | `main/src/strack.cpp` | (in trackChildWasAdded) | (via insertClip) |
| SCut wraps twSampleReader | `main/src/scut.cpp` | `getRootComponent()` | 469–481 |
| SPlainWave wraps twWavInput | `main/src/splainwave.cpp` | `getRootComponent()` | 65 |

### Parent Tracking Setup

When a component is connected, its parent must be set:

```cpp
// In parent's setInput() or wiring code:
child->setParentComponent(shared_ptr<twComponent>(this), indexInParentArray);
```

**Currently:** Parent tracking is implemented in twComponent base class but **setup at wiring time is incomplete**. This should be added at:
- `twRewire::setInput()` — when input plug is assigned
- `twMixer::setInput()` — when mixer input slot filled
- `twTrackMix::insertClip()` — when clip added
- Other component-specific wiring

## Fixes Complete: Silent Audio Root Cause Resolved

### Fix A: Release Mutex Before Recursive Render ✅
**Files:** `tw303a/src/twrewire.cc`, `tw303a/src/twmixer.cc`

Both components now snapshot input plugs/properties under brief lock, then release before calling `readStreamingData()`:
- Mutex hold time: 10-50ms (full graph render) → microseconds (just copying pointers)
- Audio callback can now immediately acquire `try_to_lock` without waiting
- Result: Audio thread never blocks on render-ahead contention

### Fix B: Make validAspects Atomic ✅
**File:** `tw303a/include/tw_output_page.h`

`validAspects` is `std::atomic<uint32_t>` with atomic load/store methods:
- Audio thread checks `page->validAspects != 0` without holding page lock
- Eliminates secondary silence window where page exists but isn't ready yet
- Result: Continuous page availability check at zero cost

## Parent Tracking Wiring: Auto-Detection ✅

**Implementation:** Parent tracking is now automatic via `setInput()`

When a child component's input is wired (`child->setInput(idx, output)`):
1. Extract parent component from the latch: `output->getParentLatch().getComponent()`
2. Store as weak_ptr to prevent circular references
3. Record input slot index (`myInputIndex_`)
4. On disconnect: clear parent tracking

**No changes to wiring code needed** — parent tracking happens transparently whenever `setInput()` is called.

**Verification:** See `tw303a/src/twcomponent.cc::setInput()` (lines 202-221)

## Known Gaps & Future Work

1. **twMoog filter state:** No `captureInternalState()` → filter state discontinuous at page boundaries (audio quality gap, not silence)
2. **Multi-track state:** `twTrackMix` stores `playOffset_` atomically but per-track state should be snapshot-based for consistency across page boundaries
3. **Integration testing:** Verify component removal during playback triggers proper silence + deregistration

## Testing the Signal Chain

### Verify Audio Output
```bash
./build.sh                       # Build
./build/bin/smaragd              # Launch
# Open/create project with audio clip
# Play → Audio should be audible
# Delete track during playback → Should fade to silence (not crash)
```

### Verify Component Hierarchy
Inspect logs for:
- `"twTrackMix: inserted clip at time ..."` → clips added to track
- `"twTrackMix: removed ... clip(s)"` → teardown called correctly
- No `"WARNING: ... found null view"` → parent tracking working

### Verify Teardown Protocol
```cpp
// In audio_engine.cc or a test:
auto track = project->getTrack(0);
track->teardown();  // Should mark all components ZOMBIE
// → audio thread detects ZOMBIE → outputs silence
// → after brief delay, components freed
```

## References

- **Architecture Design:** `plan/proposed/UNIFIED_RENDERING_ARCHITECTURE_V3.md`
- **Rendering Details:** `tw303a/include/twoutputpage.h`, `twcomponent.h` (freezePage interface)
- **Teardown Implementation:** `tw303a/src/twcomponent.cc::teardown()`
- **Parent Tracking:** `tw303a/include/twcomponent.h` (parentComponent_, myInputIndex_)
