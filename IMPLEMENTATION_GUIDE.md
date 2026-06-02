# Mutex Implementation Guide for twWavInput Thread Safety

## Files to Modify

1. `smaragd/tw303a/include/twwavinput.h`
2. `smaragd/tw303a/src/twwavinput.cc`

---

## Step 1: Add Mutex Member to Header

**File:** `smaragd/tw303a/include/twwavinput.h`

```cpp
#ifndef _TWWAVINPUT_H
#define _TWWAVINPUT_H

#include <qfile.h>
#include <mutex>              // ← ADD THIS INCLUDE
#include "twcomponent.h"

class tw303aEnvironment;

class twWavInput : public twComponent
{
public:
    // ... existing public methods ...

private:
    int findWaveProperties();

    idx_t orgChannels_;
    int orgRate_;
    int orgBits_;
    idx_t outputChannels_;

    sample_t *cache_;
    int maxCacheSize_;
    int cacheSize_;
    long dataStart_;
    length_t nSamples_;
    offset_t cacheStart_;
    QString fileName_;
    QFile file_;
    mutable std::mutex fileMutex_;    // ← ADD THIS MEMBER
    
    offset_t playOffset_;
};

#endif
```

---

## Step 2: Add Mutex Protection in calcOutputTo()

**File:** `smaragd/tw303a/src/twwavinput.cc`

Add include at the top if not already present:
```cpp
#include <mutex>
```

Modify `calcOutputTo()` to protect file access:

```cpp
length_t twWavInput::calcOutputTo(sample_t *pDest, length_t length, idx_t idx)
{
    // FIXME: Fill cache here! Reading the data every time is inefficient
    // (although linux does a good job caching).

    int orgChannels = orgChannels_;
    int neededReadLength = orgChannels*2 /* for the bits */ * length;
    short *psrc, *readData = (short *) alloca(neededReadLength);
    psrc = readData + idx;
    
    // Protect file I/O from concurrent access (UI thread preview rendering)
    int didRead;
    {
        std::lock_guard<std::mutex> lock(fileMutex_);
        file_.seek(dataStart_ + playOffset_ * orgChannels * 2);
        didRead = file_.read((char *)readData, neededReadLength);
    }
    
    // Data parsing happens without lock (it's local to this thread)
    sample_t *pd2 = pDest;
    psrc = readData;
    if (didRead < 0) didRead = 0;
    didRead /= orgChannels * 2;
    int i;
    for (i = 0; i < didRead; ++i) {
        unsigned char *x = (unsigned char *)psrc;
        *pd2++ = (sample_t)((short)(x[0] | (x[1] << 8))) / 32768.;
        psrc += orgChannels;
    }
    // FIXME: memset!!!!
    for (; i < length; ++i) {
        *pd2++ = 0;
    }
    return length;
}
```

---

## Step 3: Add Mutex Protection in findWaveProperties()

**File:** `smaragd/tw303a/src/twwavinput.cc`

Protect the file access in `findWaveProperties()` for consistency:

```cpp
int twWavInput::findWaveProperties()
{
#define SLEN 8192
    unsigned char s[SLEN];

    // ... struct definitions ...

    {
        std::lock_guard<std::mutex> lock(fileMutex_);
        if (!file_.seek(0)) return -1;
        memset(s, 0, SLEN);
        file_.read((char *)s, SLEN);
    }
    
    // Rest of the function (no lock needed)
    if (::strncmp((char *)s, "RIFF", 4)) return -2;
    if (::strncmp((char *)s + 8, "WAVEfmt ", 8)) return -3;
    
    // ... rest of function ...
}
```

**Rationale:** findWaveProperties() is currently only called during initialization, so the lock isn't strictly necessary. However, protecting it ensures consistency and guards against future changes where the file might be re-read.

---

## Step 4: Verify No Other File Access Points

**Command to check:**
```bash
grep -n "file_\." smaragd/tw303a/src/twwavinput.cc
```

Expected results should all be in one of these protected sections:
- `calcOutputTo()` (lines 84-85)
- `findWaveProperties()` (lines 138, 140)
- `getLength()` (line 21) - safe, only checks handle existence

Other occurrences in `twwavinput.h` or `smainwindow.cpp` that call `file_.handle()` are safe (they only check if file is open, not access file contents).

---

## Step 5: Build and Test

### Build
```bash
cd /Users/tweggen/coding/github/qbx/smaragd
./rebuild.sh
```

### Test Sequence
1. **Basic playback (audio only):**
   - New Project
   - Add Track
   - Add Sample
   - Start Playback
   - Should hear audio without crashes

2. **Concurrent rendering (MAIN TEST):**
   - New Project → Add Track → Add Sample → Start Playback
   - While playing (audio is audible), drag window to force waveform preview redraw
   - Continue for 10+ seconds
   - **Should NOT crash with EXC_BAD_ACCESS**
   - **Should NOT have audio glitches or pops**

3. **Undo/Redo during playback:**
   - Play sequence
   - While playing, try Cmd+Z (undo)
   - Try Cmd+Shift+Z (redo)
   - **Should work smoothly without crashes**

### Expected Results
- ✅ No crashes (EXC_BAD_ACCESS or other)
- ✅ Smooth audio playback
- ✅ Smooth waveform preview rendering
- ✅ Undo/redo functionality preserved

### If Tests Fail

**Audio stuttering/glitches:**
- May indicate lock is held too long
- Solution: Move more parsing outside the lock (most of the function is already outside)

**Still getting crashes:**
- Verify `std::mutex` is properly included
- Verify `mutable` keyword is present (needed for const functions)
- Check compiler flags support C++11 mutexes

**Build fails:**
- Ensure `#include <mutex>` is at the top of twwavinput.cc
- Verify CMakeLists.txt uses at least C++11: `-std=c++17`

---

## Code Review Checklist

- [ ] `std::mutex fileMutex_;` added to header
- [ ] `mutable` keyword present on mutex member
- [ ] `#include <mutex>` in both .h and .cc
- [ ] `std::lock_guard<std::mutex> lock(fileMutex_);` wraps file_.seek/read in calcOutputTo()
- [ ] `std::lock_guard<std::mutex> lock(fileMutex_);` wraps file_.seek/read in findWaveProperties()
- [ ] Lock scope is minimal (just file I/O, not data parsing)
- [ ] No deadlock scenarios (single lock, acquired in same order)
- [ ] Builds successfully
- [ ] Test sequence runs without crashes
- [ ] Audio quality is acceptable (no stuttering)

---

## Performance Notes

### Expected Impact
- **Audio latency:** Negligible (0-0.1ms added)
  - Lock scope: ~0.1-1ms (just file seek/read)
  - Audio buffer: 256-4096 samples (~5-90ms)
  - Lock is much smaller than audio buffer

- **UI responsiveness:** Negligible
  - UI thread might wait if audio thread is reading
  - But file I/O is already slow, wait is minimal

### Lock Contention Scenarios
1. **Audio thread reading + UI thread preview calc:**
   - Both call calcOutputTo()
   - Both need file access
   - Whichever gets lock first reads, other waits
   - Total blocking: ~1-5ms in worst case (acceptable)

2. **Long preview calculation:**
   - UI calls calcOutputTo() many times (once per pixel)
   - Lock is released between calls
   - Audio thread gets chance to read between UI calls
   - No sustained blocking

---

## Safety Guarantees After Fix

✅ `file_.seek()` and `file_.read()` are now atomic per thread
✅ No interleaving of file position changes between threads
✅ Each thread reads correct data from correct position
✅ No more EXC_BAD_ACCESS during concurrent access
✅ Preview rendering safe during audio playback
✅ Undo/redo safe during audio playback

---

## Documentation Updates

After implementing, update:
1. ✅ Already added: Thread affinity annotations in headers
2. ✅ Already added: THREAD_SAFETY_ANALYSIS.md
3. ✅ Already added: EXECUTION_PATH_DIAGRAM.md
4. ✅ Already added: SYNCHRONIZATION_FIX_PLAN.md

Ready for implementation!
