# Phase 6: Readahead Thread Page Cache Integration Fix

## Problem Statement

Silent playback occurs because frozen pages computed by the readahead thread are not discoverable by the audio callback:

```
Readahead Thread
  └─ synthOutput_->freezePage(pos, ...)
     ├─ Renders page content ✓
     ├─ Updates readaheadComputedUpTo_ ✓
     └─ Returns page to local variable (NOT CACHED) ✗

Audio Callback
  └─ synthOutput_->getPageIfExists(pageStartPos)
     └─ Searches outputPages_ cache
        └─ Page not found → returns nullptr → SILENCE ✗
```

**Root Cause:** `freezePage()` does not store the frozen page in `twComponent::outputPages_` cache.

**Evidence:** Debug logs show:
```
[AUDIO] Page MISSING: playback wants=589824, readahead computed=589824, gap=0 frames
        (readahead says it computed it, but page not found)
```

---

## Solution Architecture

### Principle
Follow the documented page cache design: `freezePage()` must cache pages before returning.

### Approach

**Option A (Recommended):** Modify `twComponent::freezePage()` to use `getOrAllocatePage()` internally

```
freezePage(startPos, length, sampleRate)
  │
  ├─ Call getOrAllocatePage(startPos) to ensure page exists in cache
  │  └─ Returns existing page OR allocates & caches new page
  │
  ├─ If page.validAspects == 0 (not yet frozen):
  │  ├─ Call freezePage_nolock() to render content
  │  ├─ Capture internal state
  │  └─ Set validAspects = twAspectPlayback (marks ready)
  │
  └─ Return page (now guaranteed to be in cache)
```

**Benefits:**
- Automatic page registration in cache
- No changes needed to readahead thread
- `getPageIfExists()` will find frozen pages immediately
- Respects existing `getOrAllocatePage()` allocation strategy

**Option B (Alternative):** Explicitly store in cache at end of `freezePage()`

```cpp
std::shared_ptr<twOutputPage> twComponent::freezePage(...) {
    std::lock_guard lock(mutex());
    auto page = std::make_shared<twOutputPage>();
    page->startPosition = startPos;
    freezePage_nolock(page, ...);
    
    // ADDITION: Store in cache
    outputPages_[startPos] = page;
    
    return page;
}
```

**Trade-off:** More explicit but duplicates `getOrAllocatePage()` logic.

---

## Implementation Plan

### Phase 6a: Fix Page Caching (CRITICAL)

**File:** `tw303a/include/twcomponent.h` and `tw303a/src/twcomponent.cc`

**Changes:**

1. **Modify `freezePage()` base implementation:**
   ```cpp
   std::shared_ptr<twOutputPage> twComponent::freezePage(
       uint64_t startPos, const sample_t *inputData, uint64_t inputOffset,
       length_t inputLength, int sampleRate,
       std::shared_ptr<twOutputPage> previousPage = nullptr
   ) {
       // Step 1: Ensure page exists in cache (allocate if needed)
       auto page = getOrAllocatePage(startPos, twAspectPlayback);
       if (!page) return nullptr;  // Allocation failed
       
       // Step 2: If already frozen with validAspects set, return it
       if (page->validAspects != 0) {
           return page;
       }
       
       // Step 3: Render the page
       {
           std::lock_guard<std::mutex> lock(mutex());
           // Release lock before recursive freezePage_nolock call
           // (mutex already held by getOrAllocatePage, so DON'T double-lock)
       }
       
       freezePage_nolock(page, startPos, inputLength, sampleRate, previousPage);
       
       // Step 4: Mark as ready
       page->validAspects = twAspectPlayback;
       
       // Step 5: Return cached page
       return page;  // Already in outputPages_ via getOrAllocatePage
   }
   ```

2. **Verify derived classes (twTrackMix, etc.):**
   - Check if custom `freezePage()` overrides properly call base class or cache pages
   - `twTrackMix::freezePage()` should continue to work (calls `freezePage_nolock()` which sets `validAspects`)

3. **Test coverage:**
   - Add assertion: `getPageIfExists(startPos)` returns non-null after `freezePage(startPos)` completes
   - Verify `validAspects != 0` in returned page

### Phase 6b: Readahead Thread Integration

**File:** `tw303a/src/audio/audio_engine.cc`

**Update `readaheadLoop()`:**

```cpp
// No changes needed! Once freezePage caches pages, readahead automatically works:
auto page = synthOutput_->freezePage(pos, nullptr, 0, pageSize, engineSampleRate_, ...);
if (page) {
    readaheadPrevPage_ = page;
    readaheadComputedUpTo_ = pos + pageSize;  // Now valid: page IS in cache
}
```

### Phase 6c: Buffer Startup Policy (FUTURE)

After caching fix validates correct behavior, implement:

1. **Minimum readahead before playback:**
   - Wait for `readaheadComputedUpTo_ >= currentPos + 3*pageSize` before allowing play to start
   - Prevents immediate underrun at playback start

2. **Underrun recovery:**
   - If playback catches readahead (`readaheadComputedUpTo_ <= currentPos`), output silence but continue
   - Allow readahead to catch up without blocking audio thread

---

## Verification & Testing

### Test 1: Page Cache Population
```cpp
// After freezePage(), page must be in cache
auto page = component->freezePage(1000, ...);
auto cached = component->getPageIfExists(1000);
ASSERT(cached != nullptr);
ASSERT(cached.get() == page.get());
```

### Test 2: Readahead Integration
```
1. Start playback
2. Wait 1 second for readahead to pre-compute pages
3. Verify readaheadComputedUpTo_ > currentPos
4. Verify [AUDIO] logs show "Page FOUND" not "Page MISSING"
5. Verify gap = readaheadComputedUpTo_ - currentPos > 0
```

### Test 3: Buffer Underrun Recovery
```
1. Seek to very end of audio
2. Start playback (readahead behind)
3. Verify [AUDIO] logs show "Page MISSING" initially
4. Wait for readahead to catch up
5. Verify page becomes available
6. Verify audio resumes (gap becomes positive)
```

### Debug Output Expectations

**Before Fix:**
```
[AUDIO] Page MISSING: playback wants=589824, readahead computed=589824, gap=0 frames ***SILENCE***
(repeated every callback)
```

**After Fix:**
```
[AUDIO] Page FOUND: playback=589824, readahead computed=851968, gap=5.46 sec
(readahead is ahead, pages available)
```

---

## Files Modified

| File | Change |
|------|--------|
| `tw303a/include/twcomponent.h` | Document caching requirement in `freezePage()` header |
| `tw303a/src/twcomponent.cc` | Implement caching in base `freezePage()` |
| `docs/ARCHITECTURE.md` | Already updated with gap documentation |

---

## Risk Assessment

### Low Risk
- `getOrAllocatePage()` already exists and works correctly
- Changes localized to base `freezePage()` implementation
- Derived classes inherit fix automatically (most don't override `freezePage()`)
- No changes to readahead or audio thread logic needed

### Potential Issues
- **Mutex double-lock:** If `getOrAllocatePage()` holds mutex and we call it from `freezePage()` which already holds mutex → deadlock
  - **Mitigation:** Check lock status; use internal `_nolock` variant if needed
  - **Current Status:** `getOrAllocatePage()` acquires lock internally, so call it BEFORE acquiring mutex

- **Performance:** Allocating pages for every position (even if not frozen yet)
  - **Mitigation:** Current design already allocates via `getOrAllocatePage()`, so neutral
  - **Benefit:** Prevents cache misses from pre-allocation

---

## Success Criteria

✅ Audio callback finds pages via `getPageIfExists()` after readahead freezes them  
✅ Gap between readahead and playback is consistently positive (buffer has cushion)  
✅ No more "Page MISSING" messages after readahead has had time to pre-compute  
✅ Audio plays continuously without silent gaps  
✅ Debug logs show `readaheadComputedUpTo_` advancing ahead of `currentPos_`  

---

## Timeline

- **Phase 6a (Page Caching):** 1-2 hours (implement + test)
- **Phase 6b (Readahead):** 0 hours (automatic, no changes needed)
- **Phase 6c (Startup Buffer):** 1-2 hours (deferred, polish feature)

---

## References

- **Architecture:** `docs/ARCHITECTURE.md` (sections on Page Cache Design, Read-Ahead Thread Integration)
- **Implementation:** `tw303a/include/twcomponent.h`, `tw303a/src/twcomponent.cc`
- **Audio Engine:** `tw303a/src/audio/audio_engine.cc`
- **Debug Output:** Implemented in commit b733647 (readahead instrumentation)
