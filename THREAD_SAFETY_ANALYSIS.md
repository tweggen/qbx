# Thread Safety Analysis: UI Redraw vs Audio Playback Race Condition

## Executive Summary

There is a **critical race condition** between the UI redraw thread and the audio playback thread, both accessing the same file handle (`QFile`) without synchronization. This causes crashes during waveform preview rendering while audio is playing.

**Shared resource:** `twWavInput::file_` (QFile instance)
**Threads involved:** UI thread (redraw) + Audio thread (CoreAudio callback)
**Impact:** EXC_BAD_ACCESS crash during waveform preview rendering

---

## Detailed Execution Paths

### Path 1: UI Thread → Preview Rendering → File Read

```
Qt Event Loop (Main/UI Thread)
  └─ SStdMixerView::paintEvent()
     └─ SMVActualView::paintEvent()
        └─ SRenderContext ctx (for each visible region)
           └─ For each visible SLink:
              └─ SLink::getObject() → SObject (typically SCut or SPlainWave)
                 └─ SObjectRenderer::draw(SLink, SRenderContext)
                    └─ SPlainWaveRendererInline::draw(SLink, SRenderContext)  [Line 13 of splainwaverndrinline.cpp]
                       └─ getPlainWave().getPreview(...) [Line 31]
                          └─ SPlainWave::getPreview(preview_t*, offset_t, length_t, offset_t)
                             └─ SObject::getStraightPreview(preview_t*, offset_t, length_t, offset_t)
                                └─ straightCalcPreviewData() [builds preview cache] (Line 135 in sobject.cpp)
                                   └─ getRootComponent().seekTo(i)
                                   └─ getRootComponent().calcOutputTo(buffer, previewSkip_, 0)
                                      └─ twWavInput::calcOutputTo(...) [Line 73 of twwavinput.cc] ⚠️ FILE ACCESS
                                         └─ file_.seek(dataStart_ + playOffset_ * ...) [Line 84]
                                         └─ file_.read((char*)readData, neededReadLength) [Line 85]
```

**Key point:** The UI thread calls `calcOutputTo()` which directly accesses `file_` (QFile).

---

### Path 2: Audio Thread → Playback Rendering → File Read

```
CoreAudio Thread (Render Callback - not Main Thread)
  └─ AudioBackend::renderCallback()
     └─ twSpeaker::readData(sample_t*, length_t)
        └─ tw303aEnvironment::calcOutputTo()
           └─ For each active component in the synth graph:
              └─ twComponent::calcOutputTo()  [typically]
                 └─ SCut::getRootComponent() → SPlainWave::getRootComponent() → twWavInput
                    └─ twWavInput::calcOutputTo(...) [Line 73 of twwavinput.cc] ⚠️ FILE ACCESS
                       └─ file_.seek(dataStart_ + playOffset_ * ...) [Line 84]
                       └─ file_.read((char*)readData, neededReadLength) [Line 85]
```

**Key point:** The audio thread also calls `calcOutputTo()` which accesses the same `file_` instance.

---

## The Race Condition

### Shared Resource Graph

```
SPlainWave (from SAddSampleAction)
  ├─ cpWave_: twWavInput*
  │  └─ file_: QFile [SHARED - NO SYNCHRONIZATION]
  ├─ fileName_: QString
  └─ previewData_: preview_t*

SCut (wrapper for SPlainWave)
  └─ content_: SLink*
     └─ getSObject() → SPlainWave
        └─ [same as above]
```

### Interleaving Scenario (Worst Case)

```
UI Thread                          | Audio Thread
-----------------------------------+-----------------------------------
[1] paintEvent() starts            |
[2] getPreview() called            |
[3] calcOutputTo() called          | [A] Audio callback triggered
    [4] file_.seek(pos1)           | [B] calcOutputTo() called
                                   | [C] file_.seek(pos2)  ⚠️ CONFLICT
[5] file_.read(...)                | [D] file_.read(...)   ⚠️ WRONG POSITION
    [6] Parse audio data @ pos1    | [E] Parse audio data @ pos2
        [7] CRASH: data corruption | [F] Audio glitch/garbage data
            or invalid access      |
```

### Specific Risk Areas

1. **File Position Race:**
   - Thread A calls `file_.seek(X)` to position at offset X
   - Thread B calls `file_.seek(Y)` before Thread A reads
   - Thread A's `file_.read()` gets data from offset Y instead of X

2. **Buffering Issues:**
   - `twWavInput::cache_` is allocated but not protected
   - Concurrent reads can corrupt the cache state

3. **QFile Handle State:**
   - QFile is not reentrant from multiple threads
   - No internal locking on file operations
   - Position tracking is thread-local in some implementations

---

## Affected Classes and Members

| Class | Member | Thread(s) | Protected? |
|-------|--------|-----------|-----------|
| `twWavInput` | `file_` | UI + Audio | ❌ NO |
| `twWavInput` | `cache_` | UI (read-only) + Audio | ⚠️ PARTIAL |
| `twWavInput` | `playOffset_` | Audio only | ✅ OK |
| `SPlainWave` | `cpWave_` | UI + Audio | ❌ NO |
| `SPlainWave` | `previewData_` | UI only | ✅ OK |
| `SCut` | `content_` | UI + Audio | ❌ NO |

---

## Current Synchronization (None)

```cpp
// twwavinput.cc:calcOutputTo() - UNPROTECTED
length_t twWavInput::calcOutputTo(sample_t *pDest, length_t length, idx_t idx)
{
    file_.seek(dataStart_ + playOffset_*orgChannels*2);  // NO MUTEX
    int didRead = file_.read((char *)readData, neededReadLength);  // NO MUTEX
    // ... parse data
}

// splainwaverndrinline.cpp:draw() - CALLS calcOutputTo() FROM UI THREAD
void SPlainWaveRendererInline::draw(SLink &lk, SRenderContext &ctx)
{
    int res = getPlainWave().getPreview(pv, o1, o2-o1, w);  // Calls calcOutputTo()
}

// sobject.cpp:straightCalcPreviewData() - CALLS calcOutputTo() FROM UI THREAD
int SObject::straightCalcPreviewData()
{
    getRootComponent().calcOutputTo(buffer, previewSkip_, 0);  // UNPROTECTED
}
```

---

## Evidence: Where the Crash Happens

From user reports:
- Crash occurs during waveform preview rendering while audio is playing
- Backtrace shows EXC_BAD_ACCESS in memory access during `calcOutputTo()`
- Only happens when both:
  1. Audio is actively playing (audio thread calling `calcOutputTo()`)
  2. Waveform is visible and being redrawn (UI thread calling `calcOutputTo()`)

---

## Solution Options

### Option A: Mutex on File Access (Recommended for Phase 2b)

```cpp
class twWavInput : public twComponent {
private:
    mutable std::mutex fileMutex_;  // Protect file_ access
    
public:
    length_t calcOutputTo(sample_t *pDest, length_t length, idx_t idx) {
        std::lock_guard<std::mutex> lock(fileMutex_);
        file_.seek(...);
        file_.read(...);
    }
};
```

**Pros:** Simple, thread-safe, minimal code changes
**Cons:** Potential audio thread latency if UI thread holds lock during preview calc

### Option B: Thread-Local File Readers

```cpp
class twWavInput : public twComponent {
private:
    QFile audioFile_;    // Audio thread only
    QFile uiFile_;       // UI thread only
    
public:
    length_t calcOutputTo(...) {
        // Use audioFile_ if in audio thread, else uiFile_
    }
};
```

**Pros:** No contention between threads, faster
**Cons:** Duplicate file handles, more complex logic

### Option C: Deferred Preview Rendering

Cache previews and invalidate only when content changes, avoiding concurrent access.

**Pros:** Reduces preview recalculations
**Cons:** Complex cache invalidation logic

---

## Recommended Fix (Phase 2b)

**Implement Option A: Mutex Protection**

1. Add `std::mutex fileMutex_` to `twWavInput`
2. Wrap all `file_` access in `lock_guard<std::mutex>`
3. Add similar protection to `twWavInput::seekTo()`
4. Test with playback + visible waveform preview rendering

**Rationale:**
- Minimal changes to existing code
- Solves the race condition completely
- Audio thread latency impact should be negligible (file I/O is already slow)

---

## Testing Checklist

- [ ] Run test sequence: Add track → Add sample → Play
- [ ] While playing, drag window to force redraw of waveform preview
- [ ] Repeat undo/redo while audio is playing
- [ ] Monitor for crashes (especially EXC_BAD_ACCESS)
- [ ] Check audio quality (no glitches, clicks, or pops)

---

## Related Code Sections

**twwavinput.cc:**
- Line 73-100: `calcOutputTo()` - FILE ACCESS
- Line 43-47: `seekTo()` - Sets playOffset_
- Line 20-25: `getLength()` - Checks file_.handle()

**sobject.cpp:**
- Line 135-191: `straightCalcPreviewData()` - Calls calcOutputTo()
- Line 193-223: `getStraightPreview()` - Called from UI thread

**splainwaverndrinline.cpp:**
- Line 13-61: `draw()` - Called from paintEvent, calls getPreview()

**smainwindow.cpp:**
- Line 180-204: `startPlaying()` - Initiates audio playback
