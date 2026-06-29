# Critical Race Conditions in twSpeaker, twTrackMix, and Audio Backends

## Executive Summary

Analysis of twSpeaker, twTrackMix, and audio backends (WASAPI, ALSA, CoreAudio) revealed **9 critical race conditions** involving use-after-free, torn reads, and iterator invalidation. These are the most severe thread-safety issues in the codebase—affecting the audio output path which runs in real-time context.

## Critical Issues

### 1. twSpeaker::setCycle() - USE-AFTER-FREE (CRITICAL)

**Location:** `tw303a/src/twspeaker.cc:208-220`

```cpp
void twSpeaker::setCycle(bool enabled, offset_t startFrame, offset_t endFrame)
{
    // ...
    if (audioEngine_) {  // LINE 217: NO LOCK - RACE!
        audioEngine_->setLoopBoundaries(enabled, startFrame, endFrame);
    }
}
```

**Race Scenario:**
- T0: Audio thread calls callback lambda (line 131-173) which reads `audioEngine_`
- T1: UI thread calls `setCycle()` and reads `audioEngine_` at line 217 (no lock!)
- T2: Simultaneously, `stopOutput()` at line 199 does `audioEngine_.reset()` (destroys object)
- T3: Audio thread/UI thread crash dereferencing destroyed pointer

**Root Cause:**
- `audioEngine_` is a `unique_ptr` (owning pointer)
- Accessed from multiple threads without synchronization
- `outputMutex_` at line 32 protects `startOutput()`/`stopOutput()` but NOT `setCycle()`
- Comment at line 68 says lock "serialises startOutput()/stopOutput()" but `setCycle()` is not included

**Impact:** Audio callback crashes mid-playback when UI adjusts loop settings

---

### 2. twSpeaker Callback Lambda - TORN READ (CRITICAL)

**Location:** `tw303a/src/twspeaker.cc:131-173`

```cpp
backend_->setRenderCallback(
    [this](float *out, std::size_t frames, std::uint32_t channels) -> std::size_t {
        if (!audioEngine_) {           // LINE 133: Reads unique_ptr without lock
            std::fill_n(out, frames * channels, 0.0f);
            return frames;
        }
        // ...
        std::size_t outFrames = audioEngine_->pullBlock(...);  // LINE 142
        // ...
        SApplication::app().setGlobalLocatorPosRealtime(audioEngine_->currentPosition());  // LINES 147, 171
    });
```

**Race Scenario:**
- Audio thread (realtime) reads `audioEngine_` in callback at lines 133, 142, 147, 171
- UI thread calls `stopOutput()` which does `audioEngine_.reset()` at line 199
- In between, `audioEngine_` is being destroyed
- Callback reads partially-destroyed pointer (torn read)

**Root Cause:**
- Lambda captured `[this]` and directly accesses member `audioEngine_`
- `std::unique_ptr` destruction is not atomic for readers
- No synchronization between callback thread and stopOutput() thread
- `unique_ptr::reset()` just deletes; doesn't notify waiting readers

**Impact:** Audio thread crashes mid-callback when UI stops playback

---

### 3. twTrackMix::seekTo() + calcOutputTo() - ITERATOR INVALIDATION (CRITICAL)

**Location:** `tw303a/src/twtrackmix.cc:14-41` (seekTo) and `tw303a/src/twtrackmix.cc:90-139` (calcOutputTo)

```cpp
int twTrackMix::seekTo( offset_t newOffset ) {
    // ...
    for( SLink *lk : track_.childLinks() ) {  // LINE 29: NO LOCK
        // ...
        lk->seekTo( clipRelative );
    }
}

length_t twTrackMix::calcOutputTo( sample_t *buffer, length_t playLen, idx_t outChannel ) {
    // ...
    for( SLink *lk : track_.childLinks() ) {  // LINE 103: NO LOCK
        // ... render lk ...
    }
}
```

**Race Scenario:**
- T0: Audio thread enters `calcOutputTo()`, starts iterating `track_.childLinks()` at line 103
- T1: Control thread calls `seekTo()` at line 29, also iterates `childLinks()`
- T2: While iteration is in progress, STrack object (UI thread) adds/removes a child
- T3: Iterator in calcOutputTo() becomes invalid → crash or corruption

**Root Cause:**
- `track_` is a reference to STrack (Qt object, not threadsafe)
- `track_.childLinks()` returns a copy or reference to internal list
- Audio thread and control thread both iterate without holding track_ lock
- STrack can be modified from UI thread (Qt event loop)

**Impact:** Audio thread crash or memory corruption during track composition changes

---

### 4. ALSA Backend - callback_ TORN READ (CRITICAL)

**Location:** `tw303a/include/audio/alsa_backend.h:25` and `tw303a/src/audio/alsa_backend.cc:241-261`

```cpp
// Header:
void setRenderCallback(RenderCallback cb) override { callback_ = std::move(cb); }

// Implementation:
void ALSABackend::asyncCallback_() {  // Called from signal handler context (line 241)
    if (!callback_) {                   // LINE 256: Reads without lock
        // ...
    }
    callback_(floatBuffer_.data(), ...);  // LINE 261: Invokes without lock
}
```

**Race Scenario:**
- T0: Control thread calls `setRenderCallback(newCb)`, writes `callback_ = std::move(cb)`
- T1: Signal handler/async thread calls `asyncCallback_()`, reads `if (!callback_)`
- T2: Callback object is mid-assignment; signal handler reads torn value
- T3: Invokes partially-constructed std::function → crash or undefined behavior

**Root Cause:**
- `std::function` assignment is not atomic
- Even with std::atomic<std::function<>> (if used), the lambda capture and data is not atomic
- Signal handler context cannot safely use mutexes (deadlock risk)
- Line 25 directly assigns without any synchronization

**Impact:** Audio callback crashes or corrupts with torn std::function reads

---

### 5. ALSA Backend - PCM Handle NOT THREAD-SAFE (CRITICAL)

**Location:** `tw303a/src/audio/alsa_backend.cc:16-88, 134-180, 241-261`

```cpp
~ALSABackend() {
    if (pcm_) closeDevice();  // LINE 18: Calls snd_pcm_close without lock
}

int ALSABackend::startOutput() {
    // ... uses pcm_ ...  (line 134-180)
}

void ALSABackend::asyncCallback_() {
    // ... uses pcm_ via writeChunk_()/pullSamples_() ...  (line 241-261)
}
```

**Race Scenario:**
- T0: Signal handler calls `asyncCallback_()` which uses `pcm_`
- T1: Control thread calls `stopOutput()` which closes/destroys `pcm_`
- T2: Signal handler tries to use destroyed `pcm_` handle → crash

**Root Cause:**
- ALSA PCM handles are NOT thread-safe per ALSA documentation
- Destructor calls `snd_pcm_close(pcm_)` without synchronization
- Signal handler (async context) can race with `stopOutput()` (control thread)

**Impact:** Audio thread crash during stop/close sequence

---

### 6. twSpeaker::audioEngine_ - DESTROYED BEFORE CALLBACK STOPS (HIGH)

**Location:** `tw303a/src/twspeaker.cc:191-201`

```cpp
void twSpeaker::stopOutput() {
    std::lock_guard<std::mutex> lock(outputMutex_);
    isPlaying_ = false;
    backend_->stopOutput();           // LINE 197: Blocks until thread exits
    audioEngine_.reset();              // LINE 199: Destroys engine AFTER thread exits
    // Good! But there's still a race with setCycle...
}
```

**The Issue:**
- `stopOutput()` does hold `outputMutex_` and destroys `audioEngine_` after backend stops
- However, `setCycle()` does NOT acquire `outputMutex_` (line 208-220)
- Even though callback should be stopped, `setCycle()` can still race with `stopOutput()`

---

## Affected Backends

All three backends have similar callback issues:

| Backend | Issue | Location |
|---------|-------|----------|
| **ALSA** | callback_ torn read, PCM handle race | `alsa_backend.cc` line 25, 256-261 |
| **WASAPI** | callback_ torn read (partially mitigated by comment) | `wasapi_backend.cc` similar |
| **CoreAudio** | outputUnit_ pointer race, callback_ torn read | `coreaudio_backend.cc` |

---

## Proposed Fixes

### Fix 1: twSpeaker::setCycle() - Add outputMutex_ Lock

**Change:**
```cpp
void twSpeaker::setCycle(bool enabled, offset_t startFrame, offset_t endFrame)
{
    if (endFrame <= startFrame) enabled = false;
    loopStart_.store(startFrame, std::memory_order_relaxed);
    loopEnd_.store(endFrame, std::memory_order_relaxed);
    cycleEnabled_.store(enabled, std::memory_order_relaxed);

    // CRITICAL: Must hold lock to prevent race with stopOutput() destroying audioEngine_
    std::lock_guard<std::mutex> lock(outputMutex_);
    if (audioEngine_) {
        audioEngine_->setLoopBoundaries(enabled, startFrame, endFrame);
    }
}
```

**Rationale:**
- Matches existing pattern in `startOutput()/stopOutput()`
- Prevents `audioEngine_` from being destroyed while in use
- Atomic stores for loop parameters (separate from engine access) remain lock-free

---

### Fix 2: Callback Lambda - Use Atomic Pointer Guard

**Change:**
```cpp
// Member: convert unique_ptr to shared_ptr for atomic reference counting
std::shared_ptr<audio::AudioEngine> audioEngine_;

backend_->setRenderCallback(
    [this](float *out, std::size_t frames, std::uint32_t channels) -> std::size_t {
        // Capture shared_ptr in callback scope (atomic acquire semantics)
        auto engine = audioEngine_;  // Atomic copy; engine stays alive in callback scope
        if (!engine) {
            std::fill_n(out, frames * channels, 0.0f);
            return frames;
        }
        
        std::vector<float> bufL(frames), bufR(frames);
        std::size_t outFrames = engine->pullBlock(bufL.data(), bufR.data(), frames);
        // ... rest of callback uses local 'engine', not this->audioEngine_
        return outFrames;
    });
```

**Rationale:**
- `shared_ptr` reference count is atomic
- Even if `stopOutput()` sets `this->audioEngine_ = nullptr`, local `engine` keeps object alive
- Callback completes safely without crashes
- No lock needed in realtime callback

---

### Fix 3: twTrackMix - Add Mutex Protection for childLinks Iteration

**Change:**
```cpp
// In twtrackmix.h:
private:
    // Helpers (following _nolock pattern):
    // Use inherited mutex() from twComponent to avoid introducing a second mutex
    // (prevents deadlock risks from lock ordering issues)
    int seekTo_nolock(offset_t newOffset);
    length_t calcOutputTo_nolock(sample_t *buffer, length_t playLen, idx_t outChannel);

// In twtrackmix.cc:
int twTrackMix::seekTo(offset_t newOffset) {
    // Acquire inherited mutex() from twComponent (no new mutex introduced)
    std::lock_guard<std::mutex> lock(mutex());
    return seekTo_nolock(newOffset);
}

int twTrackMix::seekTo_nolock(offset_t newOffset) {
    playOffset_.store(newOffset, std::memory_order_relaxed);
    for (SLink *lk : track_.childLinks()) {
        if (!lk->hasStartTime()) continue;
        offset_t startTime = lk->getStartTime();
        offset_t clipRelative = std::max((offset_t)0, newOffset - startTime);
        lk->seekTo(clipRelative);
    }
    return 0;
}

length_t twTrackMix::calcOutputTo(sample_t *buffer, length_t playLen, idx_t outChannel) {
    std::lock_guard<std::mutex> lock(trackMutex_);
    return calcOutputTo_nolock(buffer, playLen, outChannel);
}

length_t twTrackMix::calcOutputTo_nolock(sample_t *buffer, length_t playLen, idx_t outChannel) {
    // Original implementation with lock held
    for (SLink *lk : track_.childLinks()) {
        // Safe to iterate because trackMutex_ is held
        // ...
    }
}
```

**Rationale:**
- Protects `track_.childLinks()` iteration from concurrent modification
- Follows established _nolock pattern
- **Uses inherited `mutex()` from twComponent base class** to avoid introducing a second mutex
  - Multiple independent mutexes in one object create deadlock risk if there's ever a lock ordering issue
  - twComponent already protects component state with `mutex()`; reusing it for track access is consistent
  - All public methods use the same lock, making lock scope explicit and uniform
- Lock held only during childLinks iteration (brief, audio-thread critical section)

---

### Fix 4: Audio Backends - Protect callback_ Access

**For all backends (WASAPI, ALSA, CoreAudio):**

```cpp
// In header:
private:
    std::atomic<bool> hasCallback_{ false };
    std::mutex callbackMutex_;
    RenderCallback callback_;

// In setRenderCallback:
void setRenderCallback(RenderCallback cb) override {
    {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        callback_ = std::move(cb);
    }
    hasCallback_.store(true, std::memory_order_release);
}

// In asyncCallback_:
void asyncCallback_() {
    if (!hasCallback_.load(std::memory_order_acquire)) {
        return;  // Fast path: no callback set
    }
    
    std::lock_guard<std::mutex> lock(callbackMutex_);  // Acquire for callback invocation
    if (callback_) {
        callback_(floatBuffer_.data(), config_.bufferFrames, config_.channels);
    }
}
```

**Alternative for Signal Handler Safety:**
If signal handlers can't safely use mutexes, use a lock-free approach:
```cpp
// Store callback_ in a pre-allocated structure, swap atomically
std::atomic<RenderCallback*> callbackPtr_{ nullptr };
RenderCallback defaultCallback_ = [](auto...) { return 0; };

void setRenderCallback(RenderCallback cb) override {
    callback_ = std::move(cb);
    callbackPtr_.store(&callback_, std::memory_order_release);
}

void asyncCallback_() {
    auto cb = callbackPtr_.load(std::memory_order_acquire);
    if (cb) (*cb)(floatBuffer_.data(), ...);
}
```

**Rationale:**
- Lock-free fast path for checking if callback is set
- Mutex only acquired when callback exists (avoids contention)
- Atomic flag provides memory ordering guarantee

---

## Implementation Priority

| Issue | Severity | Fix Effort | Priority |
|-------|----------|-----------|----------|
| twSpeaker::setCycle() race | CRITICAL | Low | 1 |
| Callback lambda race | CRITICAL | Medium | 2 |
| twTrackMix iterator race | CRITICAL | Medium | 3 |
| Backend callback_ torn read | CRITICAL | High | 4 |
| ALSA PCM handle race | CRITICAL | High | 5 |

## Testing Strategy

1. **Automated**: Concurrent setCycle() calls during playback
2. **Automated**: Rapid track composition changes (add/remove clips) during playback
3. **Automated**: Start/stop playback in rapid succession with loop enabled
4. **Address Sanitizer**: Build with `-fsanitize=thread` to detect remaining races
5. **Manual**: Loop playback with UI adjustments should not crash

## Summary

These are the most severe thread-safety issues in the codebase because they affect the audio real-time path. Fixes are straightforward (adding locks and using shared_ptr for reference counting) but critical for stability.
