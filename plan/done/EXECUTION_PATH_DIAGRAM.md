# Execution Path Diagram: Race Condition Analysis

## Visual Timeline of Race Condition

### Scenario: Audio Playing + Waveform Visible (Crash Condition)

```
TIME → 
───────────────────────────────────────────────────────────────────────┐
                                                                        │
UI THREAD                          AUDIO THREAD (CoreAudio)            │
──────────────────────────────────────────────────────────────────────│
│
├─ [T0] startPlaying() called     │                                   │
│        twSpeaker::startOutput() │                                   │
│                                 │
├─ [T1] paintEvent() triggered    │                                   │
│        (user moved window)       │                                   │
│        │                         │                                   │
│        └─ draw()                 │                                   │
│           │                      │  [T2] CoreAudio callback         │
│           │                      │       renders block 0            │
│           │                      │       │                          │
│           │                      │       └─ calcOutputTo()          │
│           │                      │          │                       │
│           │                      │          └─ file_.seek(0)   ⚠️  │
│           │                      │             playOffset_=0        │
│           │                      │                                   │
│           └─ getPreview()        │                                   │
│              │                   │                                   │
│              └─ calcOutputTo()   │                                   │
│                 │                │  [T3] Read continues...          │
│                 │                │       but context switch         │
│                 │                │       happens HERE               │
│                 │                │                                   │
│                 └─ file_.seek()  │  [T4] Another CoreAudio block   │
│                    playOffset_=100 playOffset becomes 1024          │
│                    (for preview)  │                                   │
│                                   │       └─ file_.seek(1024)  ⚠️  │
│                                   │          OVERWRITES UI seek!    │
│
├─ [T5] Continue in UI:            │  [T6] Audio reads from          │
│        file_.read() ← WRONG POS   │       wrong position (100)      │
│        reads from 1024 instead    │       GARBAGE DATA IN AUDIO     │
│        of 100                     │                                   │
│        GARBAGE DATA IN PREVIEW    │                                   │
│        │                          │                                   │
│        └─ Parse audio @1024       │  [T7] File pointer at 1024+buf  │
│           → CRASH or corruption   │       UI expected 100+buf       │
│                                   │                                   │
└───────────────────────────────────────────────────────────────────────┘
```

---

## Call Stack at Crash Time

### State at T5 (Crash Point)

```
THREAD: UI Main
├─ paintEvent() [sstdmixerview.cpp]
│  └─ draw() [sstdmixerview.cpp] 
│     └─ SPlainWaveRendererInline::draw() [splainwaverndrinline.cpp:13]
│        └─ getPlainWave().getPreview() [splainwaverndrinline.cpp:31]
│           └─ SPlainWave::getPreview() [splainwave.cpp:getPreview]
│              └─ SObject::getStraightPreview() [sobject.cpp:193]
│                 └─ straightCalcPreviewData() [sobject.cpp:135]
│                    └─ getRootComponent().calcOutputTo()
│                       └─ twWavInput::calcOutputTo() [twwavinput.cc:73]
│                          ├─ file_.seek(100) ← SET POSITION
│                          └─ file_.read() ← READ FROM POSITION 1024 ❌ WRONG!
│                             └─ Parse invalid audio data → CRASH

THREAD: CoreAudio (Render Callback)
├─ CoreAudio Callback
│  └─ twSpeaker::readData() [twspeaker.cc]
│     └─ tw303aEnvironment::calcOutputTo()
│        └─ twComponent::calcOutputTo()
│           └─ twWavInput::calcOutputTo() [twwavinput.cc:73]
│              ├─ file_.seek(1024) ← OVERWRITES UI's seek
│              └─ file_.read() [reads from 1024, intended by audio]
```

---

## The Three-Step Race Condition

### Step 1: UI Thread Sets File Position
```cpp
// UI Thread executing in preview calculation
file_.seek(dataStart_ + playOffset_*orgChannels*2);  // seek(100)
// File pointer now at position 100
```

### Step 2: Context Switch / Thread Preemption
```
// OS scheduler preempts UI thread
// CoreAudio callback fires (might be same CPU or different)
```

### Step 3: Audio Thread Overwrites Position
```cpp
// Audio Thread executing in playback
file_.seek(dataStart_ + playOffset_*orgChannels*2);  // seek(1024)
// File pointer now at position 1024 ⚠️ OVERWRITES UI's seek
```

### Step 4: UI Thread Resumes and Reads Wrong Data
```cpp
// UI Thread resumes from where it was interrupted
int didRead = file_.read((char *)readData, neededReadLength);
// ❌ Reads from position 1024 instead of 100!
// Preview data is corrupted
```

---

## Synchronization Requirements

### Current State (UNSAFE)
```
thread affinity: NONE (both threads can access simultaneously)

QFile thread model: NOT REENTRANT
├─ File position is shared state
├─ seek() sets file pointer
├─ read() reads from current pointer
└─ No automatic synchronization between calls
```

### Needed State (SAFE)

```
thread affinity: AUDIO PRIMARY + UI SECONDARY

Protection mechanism: std::mutex fileMutex_

Protected section:
├─ file_.seek() + file_.read() must be atomic
├─ No thread can interrupt between seek() and read()
└─ twWavInput holds lock for entire operation
```

---

## Why This Happens More in Phase 2

In Phase 1, preview rendering was less common. Now with SAddSampleAction in Phase 2a:

1. Sample added → creates SPlainWave
2. Test sequence calls: Add Track → Add Sample → Play
3. Waveform appears in UI immediately
4. User sees waveform during playback
5. Playback → audio thread calls calcOutputTo()
6. UI redraw → UI thread calls calcOutputTo()
7. **RACE CONDITION OCCURS**

---

## Detection: How to Reproduce

```bash
1. Build and run smaragd
2. File → New Project
3. Run → Test Sequence (or manually: Add Track → Add Sample)
4. While audio is playing (you hear sound)
5. Drag main window or move it to force redraw
6. Listen for audio glitches
7. Watch for crashes (EXC_BAD_ACCESS)
```

Expected behavior without fix: Crash or audio corruption
Expected behavior with fix: Smooth playback + preview rendering

---

## Next Steps: Implementation

**File to modify:** `smaragd/tw303a/include/twwavinput.h` and `smaragd/tw303a/src/twwavinput.cc`

**Changes needed:**
1. Add `#include <mutex>`
2. Add member: `mutable std::mutex fileMutex_;`
3. Wrap in `lock_guard<std::mutex> lock(fileMutex_);` before file access
4. Test with playback + visible waveform

**Estimated complexity:** Low (standard mutex pattern)
**Estimated impact on audio latency:** Negligible (file I/O is already slow)
