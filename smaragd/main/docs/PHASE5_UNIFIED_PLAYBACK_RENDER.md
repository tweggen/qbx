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

### Phase 5d: Validation & Polish (weeks 7-8)
- [ ] Comprehensive testing (playback, render, both together)
- [ ] Performance profiling
- [ ] Edge cases (late-stage warmup, format changes, etc.)
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

## Open Questions for Phase 5

1. **Readahead buffer size:** 8192 frames (~170ms @ 48kHz) sufficient? Configurable?
2. **Buffering strategy:** Fixed-size ring buffer vs. demand-driven buffering?
3. **Error handling:** How to handle render underruns (vs. playback dropouts)?
4. **Network sink:** Should be included in Phase 5, or later?
5. **Legacy compatibility:** Do we maintain RenderSession for a transition period?

---

## Success Criteria

- ✅ Playback uses unified AudioEngine
- ✅ Render uses unified AudioEngine + FileSink
- ✅ No RenderSession special warmup logic
- ✅ Render handles stale captures gracefully (via readahead)
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
