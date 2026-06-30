# Backlog: Deferred Architectural Work

## Phase 4: Graph-Theory-Based freezePage() Optimization

**Status:** Deferred (postponed after Phase 3.3)  
**Rationale:** Establish logic first (Phases 1-3), optimize later

### Problem Statement

With Phase 3 complete, the rendering pipeline is unified around `freezePage()`:
- Playback uses `freezePage()` at full resolution
- Preview uses `freezePreviewPage()` at lower resolution
- Entire twComponent tree supports cascading

**However:** Current implementation has redundant freezePage() calls:
- Each consumer independently calls freezePage() on the same component
- No memoization of already-computed pages
- Example: a twTrackMix calls freezePage() on 8 child clips; if those clips are shared with other tracks, their pages are computed multiple times

### Design

#### Goal
Eliminate redundant freezePage() calls via dependency graph + memoization.

#### Approach

1. **Dependency Graph:**
   - Build directed graph: component → downstream consumers
   - Track which pages are currently frozen for which components
   - Invalidate pages when component state changes

2. **Page Cache:**
   - Store frozen pages in a per-position cache
   - When freezePage() called, check cache before computing
   - Return cached page if already materialized for this position

3. **Invalidation Strategy:**
   - When component parameters change (gain, mute, grain params):
     - Invalidate that component's cache
     - Cascade invalidation to downstream consumers
   - Atomic updates to avoid race conditions

#### Implementation Sketch

```cpp
class twComponent {
private:
    // Per-position cache: position → frozen page
    std::unordered_map<uint64_t, std::shared_ptr<twOutputPage>> pageCache_;
    
    // Dependency tracking: consumers of this component
    std::vector<twComponent*> consumers_;
    
public:
    std::shared_ptr<twOutputPage> freezePage(...) {
        // Check cache first
        if (pageCache_.count(pos)) {
            return pageCache_[pos];
        }
        
        // Compute and cache
        auto page = computeFreezePage(...);
        pageCache_[pos] = page;
        return page;
    }
    
    void invalidatePageCache() {
        pageCache_.clear();
        // Cascade to consumers
        for (auto consumer : consumers_) {
            consumer->invalidatePageCache();
        }
    }
};
```

#### Challenges

1. **Cache Size:** Pages are 256 kB each. With many positions, memory grows large.
   - Solution: LRU eviction policy, bounded cache size

2. **Thread Safety:** Multiple threads may freeze different positions concurrently.
   - Solution: per-position locks, or striped locking across cache buckets

3. **Invalidation Complexity:** Cascading invalidation must be correct but not overly broad.
   - Solution: track only truly affected positions (e.g., parameter changes affect all positions, but resampling only affects future ones)

4. **State Restoration:** Pages carrying `internalState` for stateful components (reverbs, delays).
   - Solution: previous page provided to freezePage() for state restore; cache invalidation clears historical pages

### Testing Strategy

1. Measure freezePage() call frequency (profile before/after)
2. Verify cache hits/misses under typical usage (playing, editing, previewing)
3. Confirm no audio glitches from invalidation races
4. Benchmark memory usage with and without cache

### Phase 4 Tasks

1. **Design cache invalidation policy** — when do pages become stale?
2. **Implement per-component page cache** — LRU eviction, thread-safe
3. **Wire dependency tracking** — consumers register with providers
4. **Implement cascading invalidation** — parameter changes invalidate downstream
5. **Profile and tune** — measure call frequency, cache efficiency
6. **Stress test** — rapid edits, concurrent playback + preview + export

### Integration Points

- `twComponent::freezePage()` — check cache before compute
- `twComponent::invalidate*()` — invalidate affected pages
- `setParameter()` / `setGain()` / `setMute()` — trigger invalidation
- CaptureRevalidator — coordinate invalidation with revalidation jobs

### Future Considerations

- **Distributed caching:** Cache frozen pages across network for collaborative editing?
- **Predictive loading:** Prefetch pages ahead of playback position?
- **Compression:** Store pages at lower resolution, expand on demand?

---

## Phase 5: Reintegrate freezePreviewPage() with Revalidator System

**Status:** In Progress (after Phase 3.3)  
**Rationale:** Wire preview rendering into SObject revalidation pipeline

See Phase 5 task file for current implementation.
