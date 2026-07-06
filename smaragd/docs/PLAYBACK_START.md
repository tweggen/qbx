# Playback Startup Flow

## Overview

This document describes how audio playback is initiated in Smaragd, from user action through audio output on the device.

---

## High-Level Flow (Phase 6b+: Deferred Backend Startup)

```
User clicks Play button
    ↓
SMainWindow::startPlaying()
    ├─ root->seekTo(globalPosition)           # Reset component state to play position
    ├─ syncCyclePlayback()                    # Set loop boundaries if enabled
    └─ getSpeaker()->startOutput()            # Start audio output (CRITICAL PATH)
            ↓
twSpeaker::startOutput() [NON-BLOCKING]
    ├─ Transition state: STOPPED → OPENING
    ├─ Open audio device (backend-specific)
    ├─ Negotiate sample rates across component graph
    ├─ Create new AudioEngine instance
    │   └─ audioEngine_ = std::make_unique<audio::AudioEngine>(synthOutput, rate)
    ├─ Configure resampling (engine rate ↔ device rate)
    ├─ Start readahead thread (Phase 6b)
    │   └─ audioEngine_->startReadahead()
    ├─ Register render callback (NOT STARTED YET)
    ├─ Transition state: OPENING → BUFFERING
    ├─ Spawn background task: monitorReadaheadBuffer()
    └─ Return immediately (non-blocking)
            ↓ [UI thread continues; background task runs in parallel]
            ├─ [Main thread: responsive, user can interact]
            │
            └─ monitorReadaheadBuffer() [background thread]
                ├─ Poll audioEngine_->startPlayback() every 50ms
                ├─ Wait until playback state = PLAYING (3+ sec buffered)
                ├─ Lock outputMutex_, call backend_->startOutput()
                ├─ Transition state: BUFFERING → PLAYING
                └─ Return (background thread exits)
                        ↓
            AudioEngine::readaheadLoop() [background thread, parallel]
                └─ Pre-compute frozen pages ahead of playback position
                └─ Signal playbackReadyCv_ when 3+ seconds buffered
                        ↓
            AudioEngine::pullBlock() [realtime audio callback, after PLAYING state]
                ├─ Check playback state via currentPos_
                ├─ Fetch pre-computed pages from cache
                ├─ Output audio or silence (Phase 6b: gap tolerance)
                └─ Advance playback position
                        ↓
            twSpeaker backend
                └─ Write audio to device (CoreAudio/WASAPI/ALSA)
```

**Key Improvement:** Playback starts (audio callback fires) only after readahead has buffered 3+ seconds, eliminating initial silence gap.

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

### 2. Audio Device Layer: twSpeaker::startOutput() (Phase 6b+)

**File:** `tw303a/src/twspeaker.cc` (lines 42-200)

**Responsibilities (Non-Blocking State Machine):**
- Open audio device via backend (CoreAudio/WASAPI/ALSA)
- Negotiate sample rates across component graph
- Create AudioEngine instance
- Start readahead thread
- Register (but NOT start) backend render callback
- Spawn background task to monitor readahead and defer backend startup

**Code Flow (Deferred Backend Startup):**
```cpp
void twSpeaker::startOutput() {
    std::lock_guard<std::mutex> lock(outputMutex_);
    if (isPlaying_) return;
    
    // 1. Transition to OPENING state
    outputState_.store(OutputState::OPENING, std::memory_order_relaxed);
    
    // 2. Open device
    backend_->openDevice(outputDeviceId_, graphRate);
    
    // 3. Negotiate rates
    twNegotiator negotiator(env);
    negotiator.negotiate(this, backend_->supportedRates());
    
    // 4. Get synth output component from project
    twComponent *synthOutput = &root->getRootComponent();
    
    // 5. Create AudioEngine (Tier 1: frozen page rendering)
    audioEngine_ = std::make_unique<audio::AudioEngine>(
        synthOutput, graphRate);
    
    // 6. Configure resampling (if device rate != project rate)
    audioEngine_->configureResampling(graphRate, deviceRate);
    
    // 7. START READAHEAD THREAD (Phase 6b)
    audioEngine_->startReadahead();
    
    // 8. Register render callback (DEFERRED STARTUP)
    backend_->setRenderCallback([this](...) { ... });
    
    // 9. Seek engine to current position
    audioEngine_->seekTo(currentLocatorPos);
    
    // 10. Transition to BUFFERING state
    outputState_.store(OutputState::BUFFERING, std::memory_order_relaxed);
    isPlaying_ = true;
    
    // 11. Spawn background task to monitor readahead (NON-BLOCKING)
    bufferingTaskRunning_.store(true, std::memory_order_relaxed);
    bufferingTask_ = std::thread([this] { monitorReadaheadBuffer(); });
    
    // Return immediately without calling backend_->startOutput()
}
```

**Key Design Decisions (Phase 6b+):**
- AudioEngine created fresh per startOutput() (allows device changes)
- Readahead starts automatically and pre-computes pages
- **Backend callback deferred:** `backend_->startOutput()` NOT called here
- **Non-blocking:** startOutput() returns immediately; actual playback delayed until buffered
- Background task monitors readahead progress and starts backend when ready
- State machine ensures clean transitions (OPENING → BUFFERING → PLAYING)

---

### 2b. Background Buffering Task: twSpeaker::monitorReadaheadBuffer()

**File:** `tw303a/src/twspeaker.cc` (lines 256-330)

**Responsibilities:**
- Monitor readahead progress (audioEngine_->getPlaybackState())
- Wait until playback buffer is sufficient (3+ seconds, ~144k frames)
- Call backend_->startOutput() when buffer is ready
- Handle timeout (10 seconds max) with graceful degradation

**Algorithm:**
```cpp
void twSpeaker::monitorReadaheadBuffer() {
    // Polling loop: check every 50ms if audioEngine has PLAYING state
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    
    while (bufferingTaskRunning_.load()) {
        // Check if buffer is ready (audioEngine transitioned to PLAYING)
        if (audioEngine_ && 
            audioEngine_->getPlaybackState() == audio::PlaybackState::PLAYING) {
            // Lock and start backend callback
            std::lock_guard<std::mutex> lock(outputMutex_);
            if (outputState_ == OutputState::BUFFERING) {
                backend_->startOutput();
                outputState_.store(OutputState::PLAYING);
            }
            return;
        }
        
        // Check timeout
        if (std::chrono::steady_clock::now() >= deadline) {
            fprintf(stderr, "[twSpeaker] Readahead timeout (>10 sec); stopping playback\n");
            // Gracefully stop playback
            std::lock_guard<std::mutex> lock(outputMutex_);
            outputState_.store(OutputState::STOPPED);
            isPlaying_ = false;
            backend_->closeDevice();
            audioEngine_.reset();
            return;
        }
        
        // Sleep 50ms before checking again
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}
```

**Key Features:**
- **Non-blocking polling:** Doesn't block UI thread; runs in background
- **Timeout protection:** If readahead takes >10 seconds, stops playback gracefully
- **State-driven:** Waits for AudioEngine's startPlayback() to signal PLAYING state
- **Mutex-safe:** Protects backend_->startOutput() call with outputMutex_

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

## OutputState State Machine (Phase 6b+: Deferred Backend Startup)

twSpeaker now uses a state machine to defer backend callback startup until readahead has buffered sufficiently:

```cpp
enum class OutputState {
    STOPPED = 0,    // No output, no engine
    OPENING = 1,    // Device opening, engine creating
    BUFFERING = 2,  // Readahead buffering, callback not started
    PLAYING = 3,    // Audio flowing
    STOPPING = 4    // Shutting down
};
```

**State Transitions:**
- `startOutput()`: STOPPED → OPENING → BUFFERING (then spawns background task)
- `monitorReadaheadBuffer()`: BUFFERING → PLAYING (when audioEngine achieves PLAYING state)
- `stopOutput()`: Any state → STOPPING → STOPPED
- **Timeout (10 sec):** BUFFERING → STOPPED (if readahead doesn't complete in time)

**State Machine Benefits:**
- **Clear transitions:** Each state has well-defined entry/exit conditions
- **Timeout safety:** Never waits indefinitely for buffer
- **Non-blocking:** startOutput() returns immediately; buffering happens in background
- **UI feedback:** States available via `getOutputState()` for status line display

---

## Phase 6b+: Readahead Buffer Management with Deferred Backend Startup

### Problem (Pre-Phase 6b+)
- Audio callback fires immediately after startOutput()
- Readahead thread hasn't pre-computed any pages yet
- Audio callback finds no cached pages → outputs silence for 4+ seconds
- User hears ~5 seconds of silence before audio plays

### Solution (Phase 6b+)
Three complementary mechanisms:

#### 1. Deferred Backend Startup (twSpeaker state machine)
- `startOutput()` does NOT call `backend_->startOutput()` immediately
- Instead, spawns background task `monitorReadaheadBuffer()` to wait for buffer
- Audio callback thread doesn't start until buffer is ready (~3+ seconds)
- Main thread returns immediately (non-blocking); UI stays responsive

#### 2. Skip-Ahead Optimization (readaheadLoop)
- When sequential page freeze at position N blocks, skip to N + 5*pageSize
- Reinitialize state chain (readaheadPrevPage_ = nullptr) for fresh computation
- Allows readahead to continue building buffer even if one position is slow
- Later iterations backfill the gaps

#### 3. Underrun Tolerance (updateFrozenPage)
- If gap falls below 1 second (48000 frames), output silence but continue
- Gives readahead time to replenish without blocking audio callback
- Graceful degradation instead of crash

### Measurements
- **Buffer startup:** Readahead pre-computes 3 pages (144k frames, ~3 seconds)
- **Backend startup:** Deferred until audioEngine reports PLAYING state (3+ sec buffered)
- **Buffering monitor poll:** 50ms (checks readahead progress in background task)
- **Startup timeout:** 10 seconds max before gracefully stopping playback
- **Skip distance:** 5 pages (~5.5 seconds) forward when blocked
- **Underrun threshold:** 1 second (48000 frames) before triggering silence output
- **Readahead poll interval:** 20ms (waits for signal or timeout)

---

## Thread Safety (Phase 6b+)

### Atomics (Lock-Free)
- `currentPos_` — playback position (read by audio, written by audio)
- `cycleEnabled_`, `loopStart_`, `loopEnd_` — loop settings (read by audio, written by UI)
- `readaheadRunning_` — readahead thread control flag
- `playbackState_` — Phase 6b playback state (STOPPED, BUFFERING, PLAYING)
- `outputState_` — Phase 6b+ output state machine (STOPPED, OPENING, BUFFERING, PLAYING, STOPPING)
- `bufferingTaskRunning_` — Buffering task control flag (set to false to stop monitoring)

### Mutexes (Blocking)
- `outputMutex_` — Serializes startOutput/stopOutput, and protects backend_->startOutput() call
- `readaheadMutex_` / `readaheadCv_` — Readahead thread sleep/wake (brief holds)
- Per-component mutex — Protects component state during freezePage() (released before recursive calls)

### Thread Roles
- **UI thread:** Calls startOutput()/stopOutput(), holds outputMutex_ during state changes
- **Background buffering task:** Polls readahead progress, holds outputMutex_ only to call backend_->startOutput()
- **Audio callback thread:** Non-blocking (try_to_lock), uses cached pages; starts only after BUFFERING→PLAYING
- **Readahead thread:** Can block (for freezePage), holds locks briefly; signals playbackReadyCv_ when PLAYING

### Design Principle
- **startOutput():** Non-blocking; returns immediately after spawning buffering task
- **Audio callback:** Non-blocking (doesn't start until buffer ready)
- **Readahead thread:** Can block (for freezePage), holds locks briefly
- **Buffering task:** Low-priority monitoring, 50ms poll interval prevents starvation

---

## Common Issues & Solutions

### Issue 1: Crash on startPlaying()
**Symptom:** EXC_BAD_ACCESS in stopReadahead() during startPlaying()
**Cause:** Calling getAudioEngine() before audioEngine_ is created
**Solution:** AudioEngine is created in startOutput(), not before. Readahead is started automatically there.

### Issue 2: Silent Audio at Startup (FIXED in Phase 6b+)
**Symptom (Pre-6b+):** First 4-5 seconds of playback were silent
**Cause:** Audio callback fired before readahead pre-computed pages
**Solution (Phase 6b+):** Defer backend_->startOutput() until readahead reaches PLAYING state. Audio callback now starts only when 3+ seconds are buffered. Initial silence gap eliminated.

### Issue 3: Slow Freezing (4+ seconds per page)
**Symptom:** Readahead takes 4-5 seconds to freeze a single page
**Cause:** Large component graph or I/O-bound operations (file reads, plugin processing)
**Solution:** Skip-ahead kicks in; readahead progresses to later pages while waiting. Consider async/streaming file I/O.

### Issue 4: Buffer Underruns During Playback
**Symptom:** Intermittent silence gaps during playback (not at startup)
**Cause:** Readahead falls behind playback due to graph complexity or I/O stalls
**Solution:** Phase 6b outputs silence for recovery (gap < 1 sec). Increase skip distance or pre-buffer in future.

---

## Key Files (Phase 6b+)

| File | Role |
|------|------|
| `main/src/smainwindow.cpp` | UI integration (startPlaying/stopPlaying) |
| `tw303a/include/twspeaker.h` | OutputState enum, state machine members, public getOutputState() |
| `tw303a/src/twspeaker.cc` | State machine (startOutput/stopOutput, monitorReadaheadBuffer, buffering task) |
| `tw303a/include/audio/audio_engine.h` | AudioEngine interface (readahead, pullBlock, playbackReadyCv_, getPlaybackReadyCv()) |
| `tw303a/src/audio/audio_engine.cc` | AudioEngine implementation (readaheadLoop, pullBlock, state signaling) |
| `tw303a/include/tw_output_page.h` | Frozen page structure (validAspects, startPosition, samples) |
| `tw303a/src/twcomponent.cc` | Page caching (getPageIfExists, getOrAllocatePage) |

---

## Completed Improvements (Phase 6b+)

1. ✅ **Non-blocking buffering delay:** Playback start deferred until 3+ seconds buffered (non-blocking)
2. ✅ **Output state machine:** OutputState enum with OPENING/BUFFERING/PLAYING/STOPPING states
3. ✅ **UI status line integration:** OutputState exposed via getOutputState() for "Buffering..." display
4. ✅ **Timeout safety:** 10-second max timeout with graceful playback stop if readahead stalls

## Future Improvements

1. **Direct CV signaling:** Replace polling with direct condition variable wait (more efficient)
2. **Configurable buffer sizes:** Let users tune minimum buffer duration and timeout values
3. **Progressive buffering feedback:** Show "Buffering: 30%..." progress in status line
4. **Async file I/O:** Stream WAV reads instead of pre-buffering entire files
5. **Plugin streaming:** Support real-time plugin processing without pre-computation
6. **Latency monitoring:** Expose actual buffer gaps and page freeze times to UI
7. **Early playback start:** Option to start playback before 3-second buffer (with underrun tolerance)

---

**Last Updated:** 2026-07-06 (Phase 6b+: Non-Blocking Playback Buffering State Machine Complete)
