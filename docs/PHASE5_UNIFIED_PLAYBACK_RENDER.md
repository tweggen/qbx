# Phase 5: Unified Playback/Render Architecture

## Executive Summary

**Render is playback with file buffering.** Unify the audio paths so both playback and render pull from the same component graph, handle async captures identically, and differ only in output destination (audio device vs. file buffer).

Current Problem:
- Separate RenderSession code path duplicates component graph traversal
- Special warmup/waiting logic for render vs. smooth readahead for playback
- Cache pre-warming complexity and risk of incomplete captures

Proposed Solution:
- One audio pull engine, two output sinks (device vs. file)
- Render uses same non-blocking async model as playback
- Readahead buffering handles stale captures in both paths
- File buffering allows render to wait for complete data before writing

---

## Architecture Overview

### Current State (Phase 4)

```
Playback Path:                  Render Path:
┌─────────────────┐            ┌──────────────────┐
│   Audio Device  │            │  RenderSession   │
└────────┬────────┘            └────────┬─────────┘
         │                              │
    ┌────▼─────────────┐         ┌──────▼────────────┐
    │  twSpeaker       │         │  Separate render  │
    │  (callback-based)│         │  loop (pull-based)│
    └────┬─────────────┘         └──────┬────────────┘
         │                              │
    ┌────▼──────────────────────┬──────▼──────────────┐
    │  Component Graph          │                    │
    │  (pulls captures)          │  (tries to pull    │
    └───────────────────────────┴─ from captures)    │
```

**Problems:**
- Two code paths, two maintenance points
- Render doesn't have readahead smoothing
- Async capture timing different between paths
- Warmup logic needed for render only

### Proposed State (Phase 5)

```
┌──────────────────────────────┐
│   Unified Audio Engine       │
│  (pulls from component graph)│
└──────────────────┬───────────┘
                   │
    ┌──────────────┼──────────────┐
    │              │              │
┌───▼────┐    ┌────▼────┐   ┌────▼────┐
│ Device │    │  File   │   │ Network?│
│ Sink   │    │ Sink    │   │ Sink    │
└────────┘    └─────────┘   └─────────┘
```

**Benefits:**
- One component graph pull engine
- Identical async capture handling
- Readahead buffer works for both
- Easy to add new output sinks (network, UDP, etc.)

---

## Unified Audio Engine Design

### Core Concept

```cpp
class AudioEngine {
    // Universal audio pull loop
    void pullAudioFrame(AudioFrame& output);
    
    // Both playback and render use this
    // Readahead buffer handles stale captures
    // Multiple output sinks can consume same pulled audio
};

class AudioSink {
    virtual void writeFrame(const AudioFrame&);  // Device, File, Network, etc.
};

class PlaybackSink : public AudioSink {
    // Real-time output to audio device
    // Drop frames if rendering can't keep up
};

class RenderSink : public AudioSink {
    // Buffered output to file
    // Wait for complete frames before writing
};
```

### Readahead Buffer

```cpp
class AudioReadaheadBuffer {
    // Ring buffer: fills ahead of consumption
    // Both paths read from same buffer
    // Tolerates stale/incomplete captures
    
    static const size_t READAHEAD_FRAMES = 8192;  // ~170ms @ 48kHz
    
    AudioFrame* pullFrame();    // Blocking: waits for available
    void pushFrame(AudioFrame); // Producer (component graph)
};
```

The readahead buffer is the key:
- **Playback:** Consumer runs realtime, readahead smooths over async delays
- **Render:** Consumer runs faster, buffer fills ahead, writes complete frames to disk

---

## Implementation Steps

### Step 1: Extract Unified Audio Pull

**Goal:** Create reusable component graph traversal

**Current state:**
- `twspeaker.cc`: Playback callback pulls from component graph
- `render_session.cc`: Separate render loop pulls from component graph

**Refactor:**
1. Extract common pull logic into `AudioEngine::pullFrame()`
2. Both twspeaker and RenderSession call same method
3. Single point of maintenance for component graph logic

```cpp
// tw303a/include/audio_engine.h (NEW)
class AudioEngine {
    AudioFrame pullFrame(offset_t position);
    void seekTo(offset_t position);
};

// Replaces current duplicated code in:
// - twspeaker.cc callback
// - render_session.cc render loop
```

### Step 2: Add Readahead Buffer

**Goal:** Smooth async capture timing for both paths

**Implementation:**
1. Create `AudioReadaheadBuffer` class
2. Producer (component graph pull) fills buffer
3. Consumer (playback or render) reads from buffer
4. Buffer tolerates stale captures—playback/render waits briefly if needed

```cpp
// tw303a/include/audio_readahead.h (NEW)
class AudioReadaheadBuffer {
    static const size_t READAHEAD_FRAMES = 8192;
    
    AudioFrame pullFrame(timeout_ms);  // May wait
    void pushFrame(AudioFrame);
};
```

### Step 3: Create File Sink

**Goal:** Replace RenderSession with buffered file output

**Current:** RenderSession manages render thread + file writing
**New:** FileSink handles buffered writes, using unified engine

```cpp
// tw303a/include/audio/file_sink.h (NEW)
class FileSink : public AudioSink {
    FileSink(AudioFileWriter*, size_t bufferFrames = 8192);
    
    void writeFrame(const AudioFrame&);
    void flush();  // Ensure all buffered data written
};
```

### Step 4: Unify Playback Path

**Goal:** Use unified engine in playback callback

**Changes to twspeaker.cc:**
1. Create AudioEngine instance in twSpeaker constructor
2. Callback calls `engine->pullFrame()` instead of component graph directly
3. Stream frames to DeviceSink (audio device)

### Step 5: Unify Render Path

**Goal:** Replace RenderSession with AudioEngine + FileSink

**Changes:**
1. Create AudioEngine and FileSink
2. Render thread calls `engine->pullFrame()`
3. Stream to FileSink (file buffer)
4. FileSink handles disk writes with buffering

**Before:**
```cpp
// render_session.cc
while (samplesWrittenVal < totalSamples_) {
    length_t got = resampler_.process(synthOutputPlug, buffer, want, ...);
    writer_->write(buffer, got);  // Direct write
}
```

**After:**
```cpp
// FileSink::writeFrame()
buffer_.push_back(frame);
if (buffer_.size() >= BUFFER_SIZE) {
    writer_->write(buffer_.data(), buffer_.size());
    buffer_.clear();
}
```

---

## Phase 5 Roadmap

### Phase 5a: Foundation (weeks 1-2)
- [ ] Extract AudioEngine unified pull logic
- [ ] Implement AudioReadaheadBuffer
- [ ] Create AudioSink base class
- [ ] Add DeviceSink and FileSink implementations
- [ ] Integration testing with playback

### Phase 5b: Playback Unification (weeks 3-4)
- [ ] Update twspeaker.cc to use AudioEngine
- [ ] Test playback with readahead buffer
- [ ] Verify async capture handling
- [ ] Performance benchmarking

### Phase 5c: Render Unification (weeks 5-6)
- [ ] Replace RenderSession with unified path
- [ ] Implement buffered file writing
- [ ] Test render with incomplete captures
- [ ] Stress test with large projects

### Phase 5c1: Futures Integration (within Phase 5c)
- [ ] Implement GenerationPromise RAII wrapper
- [ ] Add GenerationRegistry singleton
- [ ] Tag CapturePageData with generation ID and ready future
- [ ] Wire revalidation completion → GenerationRegistry::complete()
- [ ] Implement FileSink with future waiting logic
- [ ] Add age-based fallback (prevent forever-wait on stale data)

### Phase 5d: Validation & Polish (weeks 7-8)
- [ ] Comprehensive testing (playback, render, both together)
- [ ] Performance profiling
- [ ] Edge cases (late-stage warmup, format changes, etc.)
- [ ] Test render with incomplete captures (future timeout behavior)
- [ ] Verify no deadlocks with generation registry cleanup
- [ ] Documentation update

---

## Benefits Realized

### Code Quality
- ✅ Single audio pull engine = easier to understand and maintain
- ✅ No duplicate component graph traversal logic
- ✅ Consistent async capture handling

### Reliability
- ✅ Render no longer needs special warmup logic
- ✅ Readahead buffer smooths capture async timing
- ✅ Cache pressure eliminated (no global precompute)

### Extensibility
- ✅ New output sinks easy to add (network, UDP, JACK, etc.)
- ✅ Flexible buffering strategies per sink
- ✅ Single point for performance optimization

### Performance
- ✅ Render can run ahead, write buffered frames
- ✅ Playback has smooth readahead
- ✅ No blocking on capture completion
- ✅ Per-buffer async capture warmup integrated naturally

---

## Revalidation Completion Tracking (Futures-Based Model)

### Problem: Waiting for Captures in Render

The render path needs to ensure complete, valid captures are written to disk:

**Challenge:** Async capture revalidation is non-blocking. How does FileSink know when a buffer is ready to write?

**Options Evaluated:**
1. Qt signals: Heavy machinery (QMetaObject, event loop, marshaling) for multithreaded use
2. Condition variables: Low-level, requires manual state management
3. **std::future/std::promise: Best fit** — designed for cross-thread completion notification

### Design: Generation-Based Futures

Each revalidation cycle gets a **generation ID**. Captures are tagged with their generation. FileSink waits on futures to know when a generation is complete.

```cpp
// tw303a/include/audio/generation_promise.h (NEW)
class GenerationPromise {
    uint32_t generation_;
    std::promise<void> promise_;
    std::shared_future<void> future_;
    
public:
    GenerationPromise(uint32_t generation)
        : generation_(generation) {
        future_ = promise_.get_future().share();
    }
    
    uint32_t getGeneration() const { return generation_; }
    
    // Called when revalidation for this generation completes
    void markComplete() {
        try {
            promise_.set_value();
        } catch (const std::future_error&) {
            // Already set, idempotent
        }
    }
    
    // FileSink waits on this to know generation is stable
    std::shared_future<void> getFuture() const { return future_; }
    
    // Non-blocking check: is this generation ready?
    bool isReady(std::chrono::milliseconds timeout = std::chrono::milliseconds(0)) const {
        return future_.wait_for(timeout) == std::future_status::ready;
    }
};

// Global registry: generation ID → promise
class GenerationRegistry {
    std::unordered_map<uint32_t, std::shared_ptr<GenerationPromise>> promises_;
    std::mutex registry_mutex_;
    
public:
    std::shared_ptr<GenerationPromise> getOrCreate(uint32_t generation) {
        std::lock_guard lock(registry_mutex_);
        auto it = promises_.find(generation);
        if (it != promises_.end()) {
            return it->second;
        }
        auto promise = std::make_shared<GenerationPromise>(generation);
        promises_[generation] = promise;
        return promise;
    }
    
    void complete(uint32_t generation) {
        std::lock_guard lock(registry_mutex_);
        auto it = promises_.find(generation);
        if (it != promises_.end()) {
            it->second->markComplete();
        }
    }
};
```

### CapturePageData: Associate Generation with Audio

Each captured buffer gets tagged with its generation:

```cpp
struct CapturePageData {
    AudioFrame* frames;
    size_t frameCount;
    uint32_t generation;           // Which revalidation cycle produced this
    std::shared_future<void> readyFuture;  // Future that resolves when generation is stable
};
```

### FileSink: Wait for Stability Before Writing

```cpp
class FileSink : public AudioSink {
    struct FrameEntry {
        AudioFrame frame;
        uint32_t generation;
        std::shared_future<void> readyFuture;
        int64_t age_ms;  // Fallback: write if old enough regardless of future
    };
    
    std::deque<FrameEntry> buffer_;
    static const size_t BUFFER_SIZE = 8192;
    static const int64_t AGE_TIMEOUT_MS = 500;  // Write stale frames anyway
    
    AudioFileWriter* writer_;
    GenerationRegistry& genReg_;
    
public:
    FileSink(AudioFileWriter* writer, GenerationRegistry& genReg)
        : writer_(writer), genReg_(genReg) {}
    
    void writeFrame(const AudioFrame& frame, uint32_t generation) override {
        buffer_.push_back({
            frame,
            generation,
            genReg_.getOrCreate(generation)->getFuture(),
            getCurrentTimeMs()
        });
        
        flushWhenReady();
    }
    
private:
    void flushWhenReady() {
        while (!buffer_.empty()) {
            const auto& entry = buffer_.front();
            
            // Two conditions to write:
            // 1. Generation is stable (future resolved), OR
            // 2. Data is old enough (Age-based fallback prevents forever-wait)
            bool ready = entry.readyFuture.wait_for(std::chrono::milliseconds(0)) 
                        == std::future_status::ready;
            bool aged = (getCurrentTimeMs() - entry.age_ms) > AGE_TIMEOUT_MS;
            
            if (ready || aged) {
                writer_->write(&entry.frame, 1);
                buffer_.pop_front();
            } else {
                break;  // Stop if front frame isn't ready
            }
        }
    }
    
    void flush() override {
        // Final flush: write everything, with brief grace period
        while (!buffer_.empty()) {
            auto& entry = buffer_.front();
            entry.readyFuture.wait_for(std::chrono::milliseconds(100));
            writer_->write(&entry.frame, 1);
            buffer_.pop_front();
        }
    }
};
```

### Integration: Notify Completions After Revalidation

When revalidation cycle N completes (all dependent workers finish):

```cpp
// In SProject::finishRevalidation() or worker thread cleanup
void notifyRevalidationComplete(uint32_t generation) {
    generationRegistry.complete(generation);  // Unblock FileSink
}
```

### Why Futures over Qt Signals?

| Aspect | Qt Signals | std::future |
|--------|-----------|------------|
| **Overhead** | Marshal through event loop, QMetaObject | Direct state machine, negligible |
| **Thread-safety** | Requires Qt::QueuedConnection config | Built-in, always thread-safe |
| **Multi-waiter** | Connect multiple slots | Share one future to many readers |
| **Latency** | Event loop round-trip (~milliseconds) | Direct notification (~microseconds) |
| **Design intent** | UI coordination | Cross-thread async completion |
| **Frequency** | Few per second (UI updates) | Many per second (render buffers) |

**Decision:** Use std::future. Qt signals add unnecessary overhead in a pure audio context. Futures are designed for exactly this use case: low-latency, lock-free completion notification across threads.

### Integration with Phase 4 Lazy Invalidation

The futures model composes cleanly with Phase 4's async revalidation:

```
1. User modifies cut (e.g., starts rendering)
   ↓
2. invalidateCapture() triggered on cut + dependents
   ↓
3. New generation ID allocated: gen = ++currentGeneration_
   ↓
4. GenerationPromise created for gen, stored in registry
   ↓
5. Revalidator workers pull captures asynchronously
   (CapturePageData tagged with gen + promise->getFuture())
   ↓
6. FileSink starts buffering frames with generation IDs
   (waits on futures before writing)
   ↓
7. Last revalidator finishes → calls notifyRevalidationComplete(gen)
   ↓
8. GenerationRegistry::complete(gen) → promise.set_value()
   ↓
9. All FileSink waiters wake up → flush buffered frames for gen
```

**Key insight:** Generations and revalidation cycles are 1:1. One future per cycle, resolves when all workers complete. Multiple frames can carry the same generation; FileSink batches writes by generation stability.

### Memory Management: Registry Cleanup

Generations are transient. After FileSink flushes a generation, the promise can be discarded:

```cpp
void GenerationRegistry::forget(uint32_t generation) {
    std::lock_guard lock(registry_mutex_);
    promises_.erase(generation);
}

// In FileSink::flushWhenReady(), after writing all frames for a generation:
// ...write frames...
// genReg_.forget(generation);  // No longer needed
```

**Cleanup strategy:**
- Playback path: Forget generation after readahead buffer passes (no longer used)
- Render path: Forget generation after FileSink fully writes it to disk
- Fallback: Forget generations older than ~1 second (prevents unbounded growth)

---

## Open Questions for Phase 5

1. **Readahead buffer size:** 8192 frames (~170ms @ 48kHz) sufficient? Configurable?
2. **Generation lifecycle:** How long to keep promises in registry? (Cleanup after age-timeout)
3. **Age-based fallback:** 500ms reasonable? Adaptive based on buffer depth?
4. **Error handling:** How to handle render underruns (vs. playback dropouts)?
5. **Network sink:** Should be included in Phase 5, or later?
6. **Legacy compatibility:** Do we maintain RenderSession for a transition period?

---

## Success Criteria

- ✅ Playback uses unified AudioEngine
- ✅ Render uses unified AudioEngine + FileSink
- ✅ No RenderSession special warmup logic
- ✅ Render handles stale captures gracefully (via readahead)
- ✅ Render waits for complete captures via futures (not busy-wait or blocking)
- ✅ FileSink implements age-based fallback (write stale data if >500ms old)
- ✅ GenerationPromise is RAII-safe (no promise destruction crashes)
- ✅ Performance >= Phase 4 (or better)
- ✅ No regressions in playback quality
- ✅ Render output identical to Phase 4 (when captures are ready)

---

## Related Work

This design enables future enhancements:
- **Phase 6a:** Network sink for remote rendering / streaming
- **Phase 6b:** Real-time scrubbing with non-blocking audio pull
- **Phase 6c:** Plugin effects pipelining (same pull engine)
- **Phase 7:** GPU-accelerated DSP (single pull path to optimize)
