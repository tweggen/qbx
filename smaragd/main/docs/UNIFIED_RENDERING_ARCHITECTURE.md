# Unified Rendering Architecture

**Status:** Design & Partial Implementation  
**Date:** 2026-06-22  
**Author:** Timo Weggen, Claude  
**Scope:** Consolidate preview, playback, and export rendering into a single unified component graph pull with page caching

---

## Executive Summary

**Core Principle:** Preview, playback, and export are three output sinks for a single unified component graph rendering engine, all backed by a page cache for efficient parent-child composition.

Instead of three separate rendering paths:
- Timeline preview drawing
- Audio playback callback
- File export/render

We have **one rendering engine** with **three output destinations** and **one data flow model**:
- Component graph pull → Page cache → Output sink (Preview/Playback/Export)

This eliminates code duplication, ensures consistency, and enables efficient async rendering with proper handling of:
- Container-backed assets (groups, tracks) rendering their children
- Stale data fallbacks (no audio dropouts, no UI stalls)
- Parent-child composition via page cache hierarchy

---

## Architecture Overview

### Three-Layer Model

```
┌─────────────────────────────────────────────────────┐
│         Unified Rendering Engine                    │
│  (AudioEngine + ComponentGraph Pull + PageCache)    │
└────────────────┬──────────────────┬────────────────┘
                 │                  │
     ┌───────────┼──────────────────┼───────────┐
     │           │                  │           │
┌────▼────┐  ┌──▼────┐        ┌───▼──┐  ┌───▼────┐
│ Preview │  │Playback        │Export│  │Network?│
│  Sink   │  │ Sink           │ Sink │  │ Sink   │
└─────────┘  └───────┘        └──────┘  └────────┘
  (UI Paint)  (Audio Out)   (File Write) (Future)
```

### Data Flow: Page Cache Hierarchy

For a project with nested structure:

```
Project Root (StdMixer)
├── Track A
│   ├── Cut 1 (Sample)       → getPlaybackCapture() → SamplePage
│   └── Cut 2 (Sample)       → getPlaybackCapture() → SamplePage
└── Track B (Container Cut)
    └── Group (nested mixer)
        ├── Track C
        │   └── Cut 3 (Sample) → getPlaybackCapture() → SamplePage
        └── Track D
            └── Cut 4 (Asset)  → getPlaybackCapture() → ComposedPage

Rendering Flow:
1. Playback pulls from Project Root
2. Root queries Track A's page (mixed output of Cut 1 + Cut 2)
3. Track A's page requires Cut 1 and Cut 2 pages
4. Each cut gets its page (sample-backed → direct audio data)
5. Track A composes into single page
6. Root queries Track B's page (the container cut)
7. Container cut renders nested Group to page
8. Group queries Track C and D pages
9. Track D's Cut 4 is asset-backed (container) → renders its content
10. All composed bottom-up into Container Cut's page
```

---

## Core Components

### 1. AudioEngine (Phase 5a/5b/5c)

**Purpose:** Unified component graph pull logic  
**Status:** Implemented in Phase 5a/5b/5c

```cpp
class AudioEngine {
    // Universal audio pull: works for playback, preview, export
    AudioFrame pullFrame(offset_t position);
    
    // Loop support
    void setLoopBoundaries(offset_t start, offset_t length);
    
    // Position tracking
    offset_t currentPosition() const;
};
```

**Used by:**
- PlaybackSink (Phase 5b) → device output
- FileSink (Phase 5c) → file output
- PreviewSink (proposed) → timeline waveform

### 2. Page Cache System (Phase 4)

**Purpose:** Store rendered output of cuts/containers; enable stale-data fallback  
**Status:** Partially implemented (current/next pages, needs parent-child composition)

#### CapturePageData Structure

```cpp
struct CapturePageData {
    static const size_t PAGE_SIZE = 256 * 1024;  // 256kB per page
    
    uint8_t data[PAGE_SIZE];           // Raw audio or preview data
    uint32_t validAspects = 0;         // Bitmask: Preview|Playback|Export|Metadata
    
    // For async tracking
    std::future<bool> completionFuture;
    std::chrono::steady_clock::time_point createdAt;
};
```

#### Aspects

```cpp
enum CaptureAspect : uint32_t {
    Preview   = 1u << 0,  // Waveform peaks for timeline visualization
    Playback  = 1u << 1,  // Audio samples for real-time playback
    Metadata  = 1u << 2,  // Duration, peak levels (computed with playback)
    Export    = 1u << 3,  // Resampled audio for file export
    
    All       = Preview | Playback | Metadata | Export
};
```

### 3. CaptureRevalidator (Phase 4)

**Purpose:** Background worker threads building/updating pages  
**Status:** Implemented in Phase 4

```cpp
class CaptureRevalidator {
    // Schedule async page building for specific aspects
    void scheduleRevalidation(SCut* cut, uint32_t aspects, int priority);
    
    // Workers pull from queue and build pages
    void workerThread();
};
```

**Job Priority:**
- High (10): Playback + Metadata (audio must not drop)
- Medium (5): Export (user-initiated, not real-time)
- Low (1): Preview (UI can show stale)

### 4. Unified Sink Abstraction (Phase 5a)

**Purpose:** Flexible output destinations  
**Status:** Base class + implementations in Phase 5

```cpp
class AudioSink {
    virtual void writeFrame(const AudioFrame&) = 0;
};

class PlaybackSink : public AudioSink {
    // Real-time device output (drop frames if behind)
};

class FileSink : public AudioSink {
    // Buffered file output with futures-based completion tracking (Phase 5c)
};

class PreviewSink : public AudioSink {
    // Timeline waveform visualization (proposed)
};
```

---

## Rendering Paths (Unified)

### Path 1: Timeline Preview (UI Painting)

**Current Problem:** Container-backed cuts fail to show preview because `getContent().getPreview()` returns -1

**Solution:** Use page cache with parent-child rendering

```cpp
// In timeline paint event:
for (auto cut : visibleCuts) {
    // Non-blocking: get current/stale preview page
    auto page = cut->getPreviewCapture();
    
    if (!page) {
        // Show placeholder, schedule revalidation
        drawPlaceholder(cut);
        continue;
    }
    
    // Extract waveform peaks from page
    preview_t* peaks = reinterpret_cast<preview_t*>(page->data);
    drawWaveform(peaks, cut->getDuration());
}
```

**For Container-Backed Cuts:**
```cpp
// SCut wrapping a Track/Group:
auto page = getPreviewCapture();

if (!page || !(page->validAspects & Preview)) {
    // Schedule revalidation: render container's children to page
    revalidator_->scheduleRevalidation(this, Preview, priority=1);
    
    // Return current/stale page (may be nullptr)
    return currentPage_;
}

return page;
```

**Container Rendering (during revalidation):**
```cpp
// In CaptureRevalidator worker thread:
void revalidatePreview(SCut* containerCut) {
    auto& content = containerCut->getContent();
    
    // Render content's children via page cache
    // (Track, Group, etc. all implement getPreviewCapture())
    AudioFrame contentFrame = renderChildrenToFrame(content);
    
    // Downsample to waveform peaks
    preview_t* peaks = computePreviewPeaks(contentFrame);
    
    // Store in page cache
    containerCut->swapPages();
}
```

### Path 2: Audio Playback (Real-Time Device Output)

**Current Implementation:** Phase 5b (wired to AudioEngine)

```cpp
// In PlaybackSink callback:
void playbackSink.writeFrame(const AudioFrame& frame) {
    // Write to audio device (phase 5b)
    audioDevice_->write(frame);
}

// Unified pull loop:
while (playbackActive) {
    offset_t pos = audioEngine->currentPosition();
    
    // Render from root component via page cache
    AudioFrame frame = renderRootToFrame(pos);
    
    // Output to device via sink
    playbackSink.writeFrame(frame);
    
    audioEngine->advance();
}
```

**For Container-Backed Cuts:**
```cpp
// During renderRootToFrame():
auto page = containerCut->getPlaybackCapture();

if (!page || !(page->validAspects & Playback)) {
    // Stale data OK for playback (brief delay while revalidating)
    // Schedule: render container's children to audio page
    return currentPage_;  // May be nullptr → output silence
}

// Extract audio samples from page
float* audioData = reinterpret_cast<float*>(page->data);
return AudioFrame(audioData, page->frameCount);
```

### Path 3: File Export/Render

**Current Implementation:** Phase 5c (RenderSession refactored to use AudioEngine + FileSink)

```cpp
// In FileSink (buffered file writing):
void fileSink.writeFrame(const AudioFrame& frame) {
    buffer_.push_back(frame);
    
    if (buffer_.size() >= BUFFER_SIZE) {
        audioFileWriter_->write(buffer_);
        buffer_.clear();
    }
}

// Unified pull loop (same as playback):
while (renderActive && framesWritten < totalFrames) {
    offset_t pos = audioEngine->currentPosition();
    
    // Render from root component via page cache
    AudioFrame frame = renderRootToFrame(pos);
    
    // Output to file via sink
    fileSink.writeFrame(frame);
    
    audioEngine->advance();
}
```

**For Container-Backed Cuts:**
```cpp
// During renderRootToFrame():
auto page = containerCut->getExportCapture();

if (!page || !(page->validAspects & Export)) {
    // For export: may wait briefly for completion (user-initiated, not real-time)
    // Schedule high-priority revalidation
    return currentPage_;  // Fallback to stale
}

return AudioFrame::fromPage(*page);
```

---

## Parent-Child Composition via Page Cache

### Container Cut Rendering

A container-backed cut (e.g., asset over a track) needs to:
1. Render its content (the track/group it wraps)
2. Apply the cut's window parameters (startOffset, stretch, loop)
3. Store result in page cache

```cpp
class SCut : public SObject {
    // Get page for given aspects (preview/playback/export)
    std::shared_ptr<CapturePageData> getCapture(uint32_t aspectsMask);
    
    // Request preview page specifically
    std::shared_ptr<CapturePageData> getPreviewCapture() {
        return getCapture(Preview);
    }
    
    std::shared_ptr<CapturePageData> getPlaybackCapture() {
        return getCapture(Playback | Metadata);
    }
    
    std::shared_ptr<CapturePageData> getExportCapture() {
        return getCapture(Export | Playback | Metadata);
    }
};
```

### Revalidation Worker Building Parent Pages

When CaptureRevalidator needs to build a container cut's page:

```cpp
// In CaptureRevalidator worker thread:
void CaptureRevalidator::buildContainerPage(SCut* containerCut, uint32_t aspects) {
    auto& content = containerCut->getContent();  // Track, Group, etc.
    
    // Request content's page (recursive: may trigger children's revalidation)
    auto contentPage = content.getCapture(aspects);
    
    if (!contentPage) {
        // Content not ready yet (stale/building)
        // Mark as incomplete, will retry later
        return;
    }
    
    // Apply cut's window parameters
    length_t startOffset = containerCut->getStartOffset();
    length_t duration = containerCut->getDuration();
    double stretch = containerCut->getStretch();
    
    // Extract/transform content data
    AudioFrame contentFrame = AudioFrame::fromPage(*contentPage);
    AudioFrame windowedFrame = applyWindow(contentFrame, startOffset, duration, stretch);
    
    // Compute preview if needed
    if (aspects & Preview) {
        preview_t* peaks = computePreviewPeaks(windowedFrame);
        memcpy(page->data, peaks, sizeof(preview_t) * PREVIEW_SIZE);
    }
    
    // Store audio data if needed
    if (aspects & (Playback | Export)) {
        memcpy(page->data, windowedFrame.samples, windowedFrame.byteSize);
    }
    
    // Mark aspects as complete
    page->validAspects |= aspects;
    
    // Swap into current page
    containerCut->swapPages();
    
    // Emit signal: cut's preview/playback is ready
    emit captureRevalidated(containerCut, aspects);
}
```

### Invalidation Propagation

When a container's content changes (e.g., track muted), propagate up:

```cpp
// In STrack::setMuted():
void STrack::setMuted(bool muted) {
    muted_ = muted;
    
    // Invalidate our Playback page
    invalidateAspects(Playback | Metadata);
    
    // Notify all children (cuts) to invalidate playback
    for (auto cut : cuts_) {
        cut->invalidateAspects(Playback | Metadata);
    }
    
    // Notify parent (container cuts wrapping this track)
    notifyDependentsChanged(Playback | Metadata);
}
```

---

## Request/Response Flow for Three Sinks

### Timeline Preview Paint

```
1. paintEvent() iterates visible cuts
2. For each cut: cut->getPreviewCapture()
   
   a. Cut queries currentPage_
   b. If valid Preview: return page immediately
   c. Else: schedule revalidation (Preview, priority=1)
   
3. If page returned:
   a. Extract preview_t peaks
   b. drawWaveform(peaks)
4. Else:
   a. drawPlaceholder(cut)
   
5. [Background] Revalidator builds preview page
   a. For sample cut: compute peaks from audio
   b. For container cut: render children, compute peaks
   c. Emit signal: captureRevalidated(cut, Preview)
   
6. [Signal received] Timeline repaints
   a. Next paintEvent gets real preview
```

### Audio Playback Callback

```
1. Playback callback fires (real-time)
2. audioEngine->pullFrame(currentPos)
   a. Query component graph (root component)
   
3. Root (StdMixer) renders:
   a. For each track: track->getPlaybackCapture()
   
   b. Track queries currentPage_
   c. If valid Playback: return page immediately
   d. Else: schedule revalidation (Playback|Metadata, priority=10)
   
4. If page returned:
   a. Extract audio samples
   b. Mix into output buffer
5. Else:
   a. Output silence (no dropout, audio is continuous)
   
6. [Background] Revalidator builds playback page (high priority)
   a. For sample cut: read from source
   b. For container cut: render children recursively
   c. Store audio in page
   
7. Next callback: gets real audio (brief ~50ms delay)
```

### File Export

```
1. Export initiated: create RenderSession + FileSink
2. Export loop:
   a. audioEngine->pullFrame(currentPos)
   b. (Same as playback: pulls pages, falls back to silence)
   
3. FileSink buffers frames
   a. When buffer full: write to file
   
4. [Background] Revalidator builds Export pages
   a. Priority = 5 (medium, not real-time)
   b. Builds full-quality audio for export
   
5. If page not ready:
   a. Use stale/incomplete page (export can tolerate brief stale data)
   b. Or sleep 10ms and retry
   
6. Export completes when all frames written
```

---

## Data Structure: CapturePageData

```cpp
struct CapturePageData {
    // Raw data: audio samples or waveform peaks
    static const size_t PAGE_SIZE = 256 * 1024;
    uint8_t data[PAGE_SIZE];
    
    // Metadata
    uint32_t validAspects = 0;              // Which aspects are complete
    offset_t startOffset = 0;               // For window mapping
    length_t frameSamples = 0;              // Audio sample count in page
    int sampleRate = 48000;
    
    // Async tracking (Phase 5c)
    int generationID = 0;                   // Linked to revalidator's GenerationRegistry
    std::future<bool> completionFuture;    // Signals when ready
    
    // Age tracking (fallback logic)
    std::chrono::steady_clock::time_point createdAt;
    
    // Refcount for safe cleanup
    // (shared_ptr handles this automatically)
};
```

---

## Invalidation Model

### Aspect-Specific Invalidation

Instead of invalidating entire capture, mark specific aspects:

```cpp
// Track muted: invalidate Playback + Metadata (not Preview)
void STrack::setMuted(bool muted) {
    muted_ = muted;
    invalidateAspects(Playback | Metadata);  // Preview unaffected
}

// New sample loaded: invalidate everything
void SCut::setContent(SObject& newContent) {
    content_ = &newContent;
    invalidateAspects(All);
}
```

### Invalidation Chain

```cpp
void SCut::invalidateAspects(uint32_t aspects) {
    {
        std::lock_guard lock(mutex());
        
        // Drop stale pages
        if (aspects & Playback) {
            currentPage_.reset();
        }
        
        // Mark aspects invalid
        if (currentPage_) {
            currentPage_->validAspects &= ~aspects;
        }
    }
    
    // Schedule async revalidation
    revalidator_->scheduleRevalidation(this, aspects, priority);
    
    // Propagate to dependents (container cuts wrapping this cut)
    notifyDependentsChanged(aspects);
}
```

---

## Current Implementation Status

| Component | Status | Files |
|-----------|--------|-------|
| **Phase 4: Page Cache System** | ✅ Implemented | `capture_page_pool.h/cc`, `capture_revalidator.h/cc` |
| Phase 4: SCut two-page buffer | ✅ Implemented | `scut.h/cc` |
| Phase 4: Preview/Playback signals | ✅ Implemented | `capture_revalidator.h/cc` |
| **Phase 5a: AudioEngine Foundation** | ✅ Implemented | `audio/audio_engine.h/cc`, `audio/audio_sink.h` |
| Phase 5a: AudioReadaheadBuffer | ✅ Implemented | `audio/audio_readahead.h/cc` |
| **Phase 5b: Playback Unification** | ✅ Implemented | `twspeaker.cc`, `audio/playback_sink.h/cc` |
| **Phase 5c: Render/Export Unification** | ✅ Implemented | `render_session.cc`, `audio/file_sink.h/cc` |
| **Phase 5d: Preview Unification** | 🔨 In Progress | Proposed: `audio/preview_sink.h/cc` |
| Parent-child page composition | ⏳ Not Started | Needs: Container render logic |
| Container-backed cut preview | ⏳ Not Started | Root cause: Missing composition path |

---

## Remaining Work: Container-Backed Cut Preview

### The Problem (Found in Diagnostics)

SCut 34970960640 is a container-backed cut (wrapping a track). Its preview fails because:
1. `getPreviewCapture()` returns nullptr (no preview page built)
2. Fallback code tries `getContent().getPreview()` → returns -1 (not implemented for tracks)
3. `drawObjectWaveform()` shows nothing (not even "no preview" placeholder)

### The Solution

1. **Ensure revalidation is scheduled** when container-backed cut is encountered
2. **Implement container rendering** in CaptureRevalidator
   - Pull children's pages
   - Compose/mix them
   - Compute preview peaks
   - Store in container cut's page
3. **Signal UI to repaint** when preview page ready
4. **Show placeholder** while building (instead of invisibility)

### Implementation Steps

1. `SCut::getPreviewCapture()` must schedule revalidation for container cuts
2. `CaptureRevalidator::buildContainerPage()` must implement composition
3. `drawObjectWaveform()` must show "Asset: (building...)" while page incomplete
4. Connect `captureRevalidated` signal to trigger timeline repaint

---

## Benefits of Unified Architecture

### Code Quality
- ✅ Single component graph pull path (one source of truth)
- ✅ No duplicate rendering logic (preview/playback/export)
- ✅ Consistent async capture handling
- ✅ Parent-child composition elegantly expressed

### Reliability
- ✅ Preview no longer stalls UI (async, non-blocking)
- ✅ Playback no longer stalls audio (stale data fallback)
- ✅ Export can buffer intelligently (futures-based waiting)
- ✅ Container cuts work the same as sample cuts

### Extensibility
- ✅ Easy to add output sinks (network, JACK, etc.)
- ✅ Revalidator workers scale to CPU cores
- ✅ New visualization paths (VU meters, waveform overlays) reuse same engine

### Performance
- ✅ Preview pages cached and reused
- ✅ Playback has readahead smoothing (~170ms buffer)
- ✅ Export can run ahead of consumption
- ✅ No global precomputation blocking UI

---

## Future Extensions

Once container-backed cut preview is working:

1. **Live mixing visualization** - Preview shows real-time mix of tracks
2. **Scrubbing without blocking** - Drag playhead, preview updates asynchronously
3. **Plugin effects in preview** - Render effects chain for preview visualization
4. **Network sink** - Stream audio to remote machine
5. **GPU rendering** - Unified pull engine outputs to GPU for DSP acceleration

---

## Success Criteria

- ✅ SCut 34970960640 displays waveform preview (or "building..." placeholder)
- ✅ No UI stalls during preview/playback/export
- ✅ Audio never drops (stale data fallback works)
- ✅ Container cuts render identically in all three paths
- ✅ Revalidation happens transparently in background
- ✅ Performance ≥ Phase 4 (or better)
