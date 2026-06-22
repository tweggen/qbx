# Zero-Copy Page References: Optimization for Multi-Layer Rendering

**Problem:** Current rendering copies data at each layer:
- SPlainWave → SCut: Copy with window parameters applied
- SCut → STrack: Copy mixed output
- STrack → SGroup: Copy recursively
- SGroup → SStdMixer: Copy final mix
- SStdMixer → UI: Copy to screen

Each layer allocates temporary buffers and copies data, wasting memory bandwidth.

**Solution:** All read/process operations return `{shared_ptr<page>, offset, length}` tuples instead of copying. Callers work directly with the cached pages.

---

## Current Flow (Copy-Heavy)

```
SPlainWave::getPreview(dest, start, length, nProbes)
  ├─ Allocate temp buffer
  ├─ Read from file/memory
  ├─ Downsample to preview_t peaks
  └─ Copy into dest ← COPY #1

SCut::getPreview(dest, start, length, nProbes)
  ├─ Get parent's preview
  ├─ Apply window params (offset, stretch, loop)
  ├─ Allocate result buffer
  ├─ Copy + transform
  └─ Copy into dest ← COPY #2

STrack::renderPreview(buffer, time, length)
  ├─ For each visible clip:
  │   ├─ Get clip preview
  │   ├─ Allocate temp buffer
  │   └─ Copy into mix buffer ← COPY #3 (per clip)
  ├─ Composite all clips
  └─ Copy into output ← COPY #4

UI::drawWaveform(...)
  ├─ Get track preview
  ├─ Allocate paint buffer
  └─ Copy for rendering ← COPY #5
```

**Total:** 5+ copies for a single preview frame. For 60 FPS with 100 clips = 300+ copies/sec.

---

## Proposed Flow (Zero-Copy References)

```cpp
// All methods return PageRef instead of copying

struct PageRef {
    std::shared_ptr<CapturePageData> page;  // Shared ownership; keeps page alive
    offset_t byteOffset;                     // Offset within page.data
    length_t byteLength;                     // Valid data length
    
    // Helper: interpret as preview_t array
    const preview_t* asPreview() const {
        return reinterpret_cast<const preview_t*>(page->data + byteOffset);
    }
    
    // Helper: interpret as sample_t array
    const sample_t* asSamples() const {
        return reinterpret_cast<const sample_t*>(page->data + byteOffset);
    }
};
```

### New Interface Pattern

```cpp
// Old (copy-based)
int SPlainWave::getPreview(preview_t* dest, offset_t start, length_t length, offset_t nProbes);

// New (reference-based)
PageRef SPlainWave::getPreviewRef(offset_t start, length_t length);
```

### Rendering Flow

```
SPlainWave::getPreviewRef(time, length)
  └─ Return {page, offset_in_page, length}  ← NO COPY

SCut::getPreviewRef(time, length)
  ├─ Get parent's PageRef
  ├─ Adjust offset/length for window params
  └─ Return {same_page, adjusted_offset, adjusted_length}  ← NO COPY, just math

STrack::renderPreviewRef(time, length)
  ├─ For each visible clip:
  │   ├─ Get clip PageRef (or parent if needed)
  │   └─ Store refs (no copy)
  └─ Return composite PageRef or multi-ref  ← NO COPY, refs only

UI::drawWaveform(PageRef ref)
  └─ Read directly from ref.page->data + ref.byteOffset  ← NO COPY
```

**Total:** 0 copies. Data stays in page cache throughout pipeline.

---

## Type-Safe Page References

### Single-Ref (Simple Path)

Simple cases where one page provides all data:

```cpp
struct PageRef {
    std::shared_ptr<CapturePageData> page;
    offset_t byteOffset;
    length_t byteLength;
    uint32_t aspect;  // Preview, Playback, Metadata, or Export
};

// Usage
PageRef ref = plainWave->getPreviewRef(time, length);
if (ref.page && ref.aspect & Preview) {
    const preview_t* peaks = ref.asPreview();
    for (size_t i = 0; i < ref.byteLength / sizeof(preview_t); ++i) {
        // Use peaks[i] directly
    }
}
```

### Multi-Ref (Composition Path)

Complex cases where output is mix of multiple input refs:

```cpp
struct CompositePageRef {
    std::vector<PageRef> inputs;  // Multiple input refs
    // OR: a computed output page (if composition needed)
    std::shared_ptr<CapturePageData> outputPage;
    offset_t outputOffset;
    length_t outputLength;
    
    // Metadata about composition
    struct InputContribution {
        size_t inputIdx;
        float gainDb;
        offset_t panAmount;
        // Mix parameters
    };
    std::vector<InputContribution> contributions;
};
```

### Lazy Composition

For STrack mixing multiple clips: instead of immediately mixing, return references to all input clips. Caller (or downstream layer) does the mixing:

```cpp
CompositePageRef STrack::getPreviewRef(offset_t time, length_t length) {
    CompositePageRef result;
    
    for (SLink* clipLink : visibleClips(time)) {
        SCut* clip = dynamic_cast<SCut*>(&clipLink->getSObject());
        PageRef clipRef = clip->getPreviewRef(time, length);
        result.inputs.push_back(clipRef);  // Just store refs
    }
    
    return result;  // Caller will mix if needed
}
```

---

## Page Boundary Handling

### Single-Page Case (Common)

Data fits entirely within one page:

```cpp
PageRef getRef(offset_t time, length_t len) {
    auto page = getCapture(Preview);
    if (!page) return {};
    
    offset_t pageStartTime = ...;
    offset_t relativeOffset = (time - pageStartTime) * bytesPerSample;
    
    return {page, relativeOffset, len * bytesPerSample};
}
```

### Multi-Page Case (Rare)

Data spans multiple pages:

```cpp
struct PageRef {
    // ... existing fields ...
    
    // If spans pages, we have options:
    // Option A: Return first page, caller requests next
    // Option B: Return composite ref with multiple pages
    // Option C: Trigger revalidation to fetch next page immediately
};
```

**Recommendation:** For preview (low-res), always fits in one page. For playback/export (high-res), use pagination or lazy fetching.

---

## Memory Lifetime Semantics

### Automatic via shared_ptr

```cpp
{
    PageRef ref = track->getPreviewRef(...);
    // ref.page shared_ptr increments refcount
    
    drawWaveform(ref);  // Pass ref by const-ref
    
    // ref goes out of scope
    // shared_ptr refcount decrements
    // If no other refs, page can be recycled to pool
}
```

### No Dangling References

- shared_ptr ensures page stays alive as long as PageRef exists
- When PageRef is destroyed, pool can recycle page
- No "use-after-free" bugs possible

---

## Performance Benefits

### Memory Bandwidth

**Before:** 5 copies × 256 KB (page size) = 1.28 MB/operation
**After:** 0 copies = 0 MB/operation

For 60 FPS, 100 visible clips:
- Before: ~384 MB/sec
- After: ~0 MB/sec (just cache hits)

### CPU Cache Efficiency

- Working set stays in L3 cache (pages pre-allocated, fixed 256 KB)
- No thrashing from temporary allocations
- SIMD-friendly: caller can vectorize directly over page data

### Allocation Overhead

- Before: 100 clips × 60 FPS = 6000 allocs/sec
- After: 0 allocs/sec (pages pre-allocated)

---

## Implementation Strategy

### Phase A: Prototype (Proof of Concept)

1. Implement `PageRef` struct and `getPreviewRef()` method in SCut
2. Add `PageRef` variant to existing `getPreview()` (both paths work)
3. Update UI to use `getPreviewRef()` for one object type
4. Benchmark: compare copy-based vs reference-based

**Cost:** ~500 LOC, 1-2 days

### Phase B: Roll Out to STrack

1. Implement `getPlaybackRef()` in SPlainWave (audio-grade)
2. Implement composite mixing for STrack (multi-ref handling)
3. Update rendering pipeline to use refs
4. Benchmark nested structures

**Cost:** ~1000 LOC, 3-5 days

### Phase C: Finish SGroup/SStdMixer

1. Recursive ref composition (SGroup → SGroup)
2. Master effects pipeline
3. Full integration testing

**Cost:** ~500 LOC, 2-3 days

### Phase D: Optimize Composition

1. SIMD-friendly mixing loops (read from multi-refs in parallel)
2. Page cache tuning (adjust size based on typical workload)
3. Lazy vs eager mixing decisions

**Cost:** Variable, depends on profiling

---

## Design Decisions

### When to Use PageRef vs Copy

| Use Case | Method | Return |
|----------|--------|--------|
| Simple leaf node (SPlainWave) | `getPreviewRef()` | `PageRef` (no copy) |
| Single input transform (SCut) | `getPlaybackRef()` | `PageRef` (no copy) |
| Multi-input composite (STrack) | `renderRef()` | `CompositePageRef` (refs only) |
| Master effects (SStdMixer) | `getFinalRef()` | `PageRef` (may be new computed page) |
| UI rendering | Use `PageRef` directly | Zero-copy to screen buffer |

### Backward Compatibility

Keep old `getPreview(dest, ...)` methods as thin wrappers:

```cpp
int SPlainWave::getPreview(preview_t* dest, offset_t start, length_t len, offset_t nProbes) {
    // Old interface: still supported but uses new internals
    PageRef ref = getPreviewRef(start, len);
    if (!ref.page) return -1;
    
    memcpy(dest, ref.asPreview(), ref.byteLength);  // Single copy
    return ref.byteLength / sizeof(preview_t);
}
```

Allows gradual migration; old code keeps working.

---

## Challenges & Solutions

| Challenge | Solution |
|-----------|----------|
| **Type safety** (preview_t vs sample_t in same page) | Use `aspect` bitmask + helper methods (`asPreview()`, `asSamples()`) |
| **Multi-page spans** | For preview: always fits in one page. For audio: fetch next page on demand. |
| **Compositing without copies** | Return `CompositePageRef` with input refs + mix parameters. Caller/downstream does mixing. |
| **Backward compatibility** | Keep old copy-based methods as thin wrappers around new ref methods. |
| **Lifetime management** | shared_ptr handles it automatically. No manual cleanup. |
| **Cache invalidation** | Same as before: revalidator marks page invalid, next getRef() triggers revalidation. |

---

## Success Criteria

- ✅ Zero copies in main rendering path (preview, playback, export)
- ✅ PageRef fully specifies data (offset, length, type)
- ✅ shared_ptr ensures page lifetime
- ✅ Composition works with multi-ref (STrack mixing)
- ✅ Backward compat: old copy-based code still works
- ✅ Performance: measurable bandwidth reduction (should be obvious)
- ✅ No regression in latency (refs as fast as copies)

---

## Rollback Plan

If performance doesn't improve or breaks something:
1. Revert to copy-based methods
2. Keep PageRef as alternate API
3. Profile to understand bottleneck

(Likely won't need rollback; bandwidth savings are real.)

---

## Comparison with Alternative Approaches

| Approach | Pros | Cons |
|----------|------|------|
| **PageRef (proposed)** | Zero-copy, zero-alloc, automatic lifetime | Refactoring needed, composition complexity |
| **Unsafe pointers** | Same perf, less refactoring | Dangling pointers, manual lifetime |
| **Copy-on-write** | Lazy copying | Still has copies in practice |
| **Memory pools** | Reduces alloc overhead | Doesn't eliminate copies |
| **SIMD mixing** | Faster mixing within page | Doesn't eliminate copies from reads |

**PageRef is best:** Zero-copy with safe automatic lifetime.

---

## Integration with Existing Systems

### Page Cache (Phase 5d–5e)

- ✅ Pages already pre-allocated (CapturePagePool)
- ✅ Pages already thread-safe (atomic_load/store, pageMutex)
- ✅ Aspects already tracked (validAspects bitmask)
- ✅ PageRef just exposes existing pages directly

### CaptureRevalidator

- ✅ No changes needed
- ✅ Still manages pages the same way
- ✅ Just populates page data; callers consume differently

### Audio Engine

- ✅ playback/export can use PageRef too
- ✅ Different aspect types (Playback, Export) in same page
- ✅ Mixing logic unchanged; just reads from refs instead of buffers

---

## Timeline

**Phase A (Prototype):** 1–2 days → Proof it works
**Phase B (STrack):** 3–5 days → Main benefit unlocked
**Phase C (Hierarchy):** 2–3 days → Complete integration
**Phase D (Optimize):** 1–2 days → Fine-tune performance

**Total:** ~1 week to full zero-copy pipeline.

Very low risk; can be done incrementally and reverted easily.
