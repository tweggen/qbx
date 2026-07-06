# Playback Startup Flow

## Overview

This document describes how audio playback is initiated in Smaragd, from user action through audio output on the device.

---

## High-Level Flow

```
User clicks Play button
    ↓
SMainWindow::startPlaying()
    ├─ root->seekTo(globalPosition)           # Reset component state to play position
    ├─ syncCyclePlayback()                    # Set loop boundaries if enabled
    └─ getSpeaker()->startOutput()            # Start audio output (CRITICAL PATH)
            ↓
twSpeaker::startOutput()
    ├─ Open audio device (backend-specific)
    ├─ Negotiate sample rates across component graph
    ├─ Create new AudioEngine instance
    │   └─ audioEngine_ = std::make_unique<audio::AudioEngine>(synthOutput, rate)
    ├─ Configure resampling (engine rate ↔ device rate)
    ├─ Start readahead thread (Phase 6b)
    │   └─ audioEngine_->startReadahead()
    └─ Start backend audio callback thread
            ↓
AudioEngine::readaheadLoop() [background thread]
    └─ Pre-compute frozen pages ahead of playback position
            ↓
AudioEngine::pullBlock() [realtime audio callback]
    ├─ Check playback state via currentPos_
    ├─ Fetch pre-computed pages from cache
    ├─ Output audio or silence (Phase 6b: gap tolerance)
    └─ Advance playback position
            ↓
twSpeaker backend
    └─ Write audio to device (CoreAudio/WASAPI/ALSA)
```

---

## Detailed Component Interactions

### 1. UI Layer: SMainWindow::startPlaying()

**File:** `main/src/smainwindow.cpp` (lines 478-505)

**Responsibilities:**
- Toggle between play/stop states
- Seek to current locator position
- Arm cycle (loop) playback region
- Delegate to speaker for audio output

**Code Flow:**
```cpp
void SMainWindow::startPlaying() {
    if (SApplication::app().isPlaying()) {
        // Already playing; stop instead
        getSpeaker()->stopOutput();
    } else {
        // Prepare playback
        SObject *root = currentProject_->getRootComponent();
        root->seekTo(SApplication::app().getGlobalLocatorPos());
        syncCyclePlayback();  // Set loopStart_, loopEnd_, cycleEnabled_
        
        // START AUDIO OUTPUT
        SApplication::app().getSpeaker()->startOutput();
    }
}
```

**Critical Points:**
- Seeks component graph to current position (resets internal state)
- Loop boundaries must be set BEFORE startOutput() (so audio thread sees them)
- startOutput() is blocking until device opens and callback thread starts

---

### 2. Audio Device Layer: twSpeaker::startOutput()

**File:** `tw303a/src/twspeaker.cc` (lines 42-190)

**Responsibilities:**
- Open audio device via backend (CoreAudio/WASAPI/ALSA)
- Negotiate sample rates across component graph
- Create AudioEngine instance
- Start readahead thread
- Launch backend render callback thread

**Code Flow (Simplified):**
```cpp
void twSpeaker::startOutput() {
    // 1. Open device
    backend_->openDevice(outputDeviceId_, graphRate);
    
    // 2. Negotiate rates
    twNegotiator negotiator(env);
    negotiator.negotiate(this, backend_->supportedRates());
    
    // 3. Get synth output component from project
    twComponent *synthOutput = &root->getRootComponent();
    
    // 4. Create AudioEngine (Tier 1: frozen page rendering)
    audioEngine_ = std::make_unique<audio::AudioEngine>(
        synthOutput, graphRate);
    
    // 5. Configure resampling (if device rate != project rate)
    audioEngine_->configureResampling(graphRate, deviceRate);
    
    // 6. START READAHEAD THREAD (Phase 6b)
    audioEngine_->startReadahead();
    
    // 7. Start backend callback thread
    backend_->startOutput();
    
    isPlaying_ = true;
}
```

**Key Design Decisions:**
- AudioEngine created fresh per startOutput() (allows device changes)
- Readahead starts automatically (no UI integration needed)
- Backend callback is non-blocking (uses try_to_lock pattern)

---

### 3. Readahead Thread: AudioEngine::readaheadLoop()

**File:** `tw303a/src/audio/audio_engine.cc` (lines 373-480)

**Responsibilities:**
- Monitor current playback position
- Pre-compute frozen pages ahead (buffer for ~3-5 seconds)
- Implement skip-ahead optimization (Phase 6b)
- Gate progress updates on validAspects confirmation

**Algorithm:**
```cpp
void AudioEngine::readaheadLoop() {
    const int READAHEAD_PAGES = 3;        // 3 pages = ~144k frames = 3 sec at 48kHz
    const int SKIP_DISTANCE = 5;          // Skip to page+5 if blocked (Phase 6b)
    
    while (readaheadRunning_) {
        // Wait 20ms or signal
        std::unique_lock<std::mutex> lk(readaheadMutex_);
        readaheadCv_.wait_for(lk, std::chrono::milliseconds(20));
        lk.unlock();
        
        uint64_t currentPos = currentPos_.load();
        uint64_t pageStart = (currentPos / pageSize) * pageSize;
        
        // Detect seek: reset state chain if playhead jumped far
        if (pageStart < readaheadComputedUpTo_ ||
            pageStart > readaheadComputedUpTo_ + READAHEAD_PAGES * pageSize) {
            readaheadPrevPage_ = nullptr;
        }
        
        // Try to freeze READAHEAD_PAGES (3) pages ahead
        for (int i = 0; i < READAHEAD_PAGES; i++) {
            uint64_t pos = pageStart + i * pageSize;
            if (pos < readaheadComputedUpTo_) continue;
            
            // TRY SEQUENTIAL: freeze with existing state chain
            auto page = synthOutput_->freezePage(pos, nullptr, 0, pageSize,
                                                 engineSampleRate_,
                                                 readaheadPrevPage_);
            
            if (page && page->validAspects != 0) {
                // Success: page is frozen and ready
                readaheadPrevPage_ = page;
                readaheadComputedUpTo_ = pos + pageSize;  // GATED ON validAspects
                continue;
            }
            
            // TRY SKIP-AHEAD (Phase 6b): if sequential blocked, skip forward
            if (readaheadPrevPage_ != nullptr) {
                uint64_t skipPos = pos + SKIP_DISTANCE * pageSize;
                auto skipPage = synthOutput_->freezePage(skipPos, nullptr, 0, pageSize,
                                                         engineSampleRate_,
                                                         nullptr);  // Fresh state
                if (skipPage && skipPage->validAspects != 0) {
                    readaheadPrevPage_ = skipPage;
                    readaheadComputedUpTo_ = skipPos + pageSize;
                    continue;  // Next iteration tries skipPos+pageSize
                }
            }
            
            // Both failed; give up this iteration
            break;
        }
    }
}
```

**Phase 6b Features:**
- **Progress gating:** Only claims frozen pages (validAspects != 0)
- **Skip-ahead optimization:** If page N blocks, skip to N+5*pageSize with fresh state
- **Graceful degradation:** Breaks and retries in next 20ms iteration

---

### 4. Audio Callback: AudioEngine::pullBlock()

**File:** `tw303a/src/audio/audio_engine.cc` (lines 51-157)

**Responsibilities:**
- Pull audio frames from cached frozen pages
- Handle resampling if device rate ≠ project rate
- Detect underruns (gap < 1 sec) and output silence
- Never block (try_to_lock on page cache)

**Algorithm:**
```cpp
length_t AudioEngine::pullBlock(float* outL, float* outR, length_t nFrames) {
    // Resample or passthrough to get frames from frozen pages
    
    // Simplified: passthrough case (device rate == project rate)
    length_t produced = 0;
    for (length_t i = 0; i < nFrames; ++i) {
        if (!pullStereoFrameFrozen(outL[i], outR[i])) {
            break;  // No more frames available
        }
        produced++;
    }
    
    // Zero any remaining frames (silence output if underrun)
    if (produced < nFrames) {
        std::memset(outL + produced, 0, (nFrames - produced) * sizeof(float));
        std::memset(outR + produced, 0, (nFrames - produced) * sizeof(float));
    }
    
    return produced;  // Inform backend how many frames were produced
}

bool AudioEngine::pullStereoFrameFrozen(float& outL, float& outR) {
    // Get current playback position
    uint64_t pos = currentPos_.load();
    
    // Handle loop wrapping (if cycle enabled)
    if (cycleEnabled_ && pos >= loopEnd_) {
        pos = loopStart_;
    }
    
    // Update frozen page cache if we've moved to a new page
    updateFrozenPage(pos);
    
    if (!currentFrozenPage_) {
        outL = outR = 0.0f;  // No page available; silence
        return false;
    }
    
    // Extract sample from frozen page
    float sample = currentFrozenPage_->samples[pageFrameOffset_];
    outL = outR = sample;
    
    pageFrameOffset_++;
    currentPos_.store(pos + 1, std::memory_order_relaxed);
    return true;
}

void AudioEngine::updateFrozenPage(uint64_t desiredPos) {
    uint64_t pageSize = twOutputPage::FRAME_CAPACITY;
    uint64_t pageStartPos = (desiredPos / pageSize) * pageSize;
    
    // Try to get page from cache (non-blocking)
    auto page = synthOutput_->getPageIfExists(pageStartPos);
    
    if (page && page->validAspects != 0) {
        // Page is ready; use it
        currentFrozenPage_ = page;
        currentPageStartPos_ = pageStartPos;
        pageFrameOffset_ = desiredPos - pageStartPos;
    } else {
        // Page not ready
        int64_t gap = readaheadComputedUpTo_ - pageStartPos;
        
        if (gap >= 0 && gap < UNDERRUN_THRESHOLD_FRAMES) {
            // Phase 6b: Gap < 1 sec; output silence for recovery
            // Readahead catches up without audio thread blocking
        } else if (gap < 0) {
            // Serious issue: readahead behind playback
        }
        
        currentFrozenPage_ = nullptr;
        readaheadCv_.notify_one();  // Wake readahead thread
    }
}
```

**Critical Guarantees:**
- **Non-blocking:** Uses try_to_lock; never waits on mutex
- **Underrun tolerance:** Phase 6b outputs silence if gap < 1 second
- **Loop aware:** Respects cycleEnabled_/loopStart_/loopEnd_ atomics

---

## Phase 6b: Readahead Buffer Management

### Problem (Pre-Phase 6b)
- Audio callback fires immediately after startOutput()
- Readahead thread hasn't pre-computed any pages yet
- Audio callback finds no cached pages → outputs silence
- Buffer gap shows negative (readahead far behind playback)

### Solution (Phase 6b)
Two complementary mechanisms:

#### 1. Skip-Ahead Optimization (readaheadLoop)
- When sequential page freeze at position N blocks, skip to N + 5*pageSize
- Reinitialize state chain (readaheadPrevPage_ = nullptr) for fresh computation
- Allows readahead to continue building buffer even if one position is slow
- Later iterations backfill the gaps

#### 2. Underrun Tolerance (updateFrozenPage)
- If gap falls below 1 second (48000 frames), output silence but continue
- Gives readahead time to replenish without blocking audio callback
- Graceful degradation instead of crash

### Measurements
- **Buffer startup:** Readahead pre-computes 3 pages (144k frames, ~3 seconds)
- **Skip distance:** 5 pages (~5.5 seconds) forward when blocked
- **Underrun threshold:** 1 second (48000 frames) before triggering silence output
- **Readahead poll interval:** 20ms (waits for signal or timeout)

---

## Thread Safety

### Atomics (Lock-Free)
- `currentPos_` — playback position (read by audio, written by audio)
- `cycleEnabled_`, `loopStart_`, `loopEnd_` — loop settings (read by audio, written by UI)
- `readaheadRunning_` — readahead thread control flag
- `playbackState_` — Phase 6b playback state (STOPPED, BUFFERING, PLAYING)

### Mutexes (Blocking)
- `outputMutex_` — Serializes startOutput/stopOutput (no callback thread access)
- `readaheadMutex_` / `readaheadCv_` — Readahead thread sleep/wake (brief holds)
- Per-component mutex — Protects component state during freezePage() (released before recursive calls)

### Design Principle
- **Audio callback:** Non-blocking (try_to_lock), uses cached pages
- **Readahead thread:** Can block (for freezePage), holds locks briefly
- **UI thread:** Can block (startOutput/stopOutput), uses lock while changing state

---

## Common Issues & Solutions

### Issue 1: Crash on startPlaying()
**Symptom:** EXC_BAD_ACCESS in stopReadahead() during startPlaying()
**Cause:** Calling getAudioEngine() before audioEngine_ is created
**Solution:** AudioEngine is created in startOutput(), not before. Readahead is started automatically there.

### Issue 2: Silent Audio at Startup
**Symptom:** First 2-3 seconds of playback are silent
**Cause:** Readahead hasn't pre-computed pages by the time audio callbacks fire
**Solution:** Phase 6b skip-ahead and underrun tolerance. May delay playback start in future.

### Issue 3: Slow Freezing (4+ seconds per page)
**Symptom:** Readahead takes 4-5 seconds to freeze a single page
**Cause:** Large component graph or I/O-bound operations (file reads, plugin processing)
**Solution:** Skip-ahead kicks in; readahead progresses to later pages while waiting. Consider async/streaming file I/O.

### Issue 4: Buffer Underruns During Playback
**Symptom:** Intermittent silence gaps during playback (not at startup)
**Cause:** Readahead falls behind playback due to graph complexity or I/O stalls
**Solution:** Phase 6b outputs silence for recovery (gap < 1 sec). Increase skip distance or pre-buffer in future.

---

## Key Files

| File | Role |
|------|------|
| `main/src/smainwindow.cpp` | UI integration (startPlaying/stopPlaying) |
| `tw303a/src/twspeaker.cc` | Device layer (startOutput/stopOutput, audioEngine lifecycle) |
| `tw303a/include/audio/audio_engine.h` | AudioEngine interface (readahead, pullBlock, page cache) |
| `tw303a/src/audio/audio_engine.cc` | AudioEngine implementation (readaheadLoop, pullBlock, gap management) |
| `tw303a/include/tw_output_page.h` | Frozen page structure (validAspects, startPosition, samples) |
| `tw303a/src/twcomponent.cc` | Page caching (getPageIfExists, getOrAllocatePage) |

---

## Future Improvements

1. **Minimum buffering delay:** Delay playback start until readahead has 3-second cushion (UI-blocking)
2. **Playback state enum:** Expose BUFFERING/PLAYING states to UI for progress indication
3. **Configurable buffer sizes:** Let users tune skip distance and underrun threshold
4. **Async file I/O:** Stream WAV reads instead of pre-buffering entire files
5. **Plugin streaming:** Support real-time plugin processing without pre-computation
6. **Latency monitoring:** Expose actual buffer gaps and page freeze times to UI

---

**Last Updated:** 2026-07-06 (Phase 6b: Readahead Buffer Management Complete)
