# Component Locking Strategy: The _nolock() Pattern

## Overview

Smaragd uses an explicit **_nolock() pattern** for thread-safe component access instead of recursive mutexes. This strategy prevents deadlocks while making lock scope visible and maintainable.

## Why Not Recursive Mutexes?

Recursive mutexes (std::recursive_mutex) allow the same thread to acquire the same lock multiple times. While convenient, they **hide logical flaws**:

- A function acquires a lock, then calls another function that also tries to acquire the same lock
- With recursive_mutex, this silently succeeds—masking the underlying design problem
- Race conditions and deadlocks can still occur; the lock just doesn't prevent them
- The code becomes harder to reason about: you never know if a lock is held when reading a function

**Example of hidden danger:**
```cpp
// Bad: recursive_mutex hides the problem
std::recursive_mutex m;

void updateComponent() {
  std::lock_guard<std::recursive_mutex> lock(m);
  // ... work ...
  checkDependents();  // Also acquires m (silently succeeds!)
}

void checkDependents() {
  std::lock_guard<std::recursive_mutex> lock(m);  // Acquires same lock again
  // If updateComponent held ANY invariant during checkDependents, it's violated
}
```

With recursive_mutex, this compiles and runs. But if `checkDependents()` is ever called from elsewhere without the lock, or if it's refactored to release the lock temporarily, race conditions appear.

## The _nolock() Pattern

**Core principle:** Explicitly split functions into two variants:

1. **Public function (no suffix):** Acquires the mutex, does minimal work, releases before calling expensive operations
2. **Private function (_nolock suffix):** Assumes the mutex is already held, performs the actual work

```cpp
// Public API: acquires lock briefly
void twComponent::updateState() {
  std::lock_guard<std::mutex> lock(mutex());
  
  // Do cache checks, quick operations under lock
  if (alreadyComputed()) {
    return;  // Can return early
  }
  
  // Allocate placeholder and release lock
  auto page = allocatePlaceholder();
  // Lock released here
  
  // Release lock explicitly before calling expensive operation
}

// Private helper: assumes lock is held (or not needed)
void twComponent::updateState_nolock(std::shared_ptr<Page> page) {
  // Caller must hold mutex() OR this is called after releasing it
  // This function does NOT acquire the lock
  
  // Safe for recursive calls because we don't try to acquire the same lock
  renderPage(page);
}
```

## Benefits

### 1. **Explicit Lock Scope**
Reading the code makes it clear when locks are held:
```cpp
{
  std::lock_guard<std::mutex> lock(mutex());
  // Lock is held here
}
// Lock released here
// Safe to call functions that might need the lock
```

### 2. **Prevents Deadlock**
Recursive operations can safely call _nolock variants without acquiring the same lock twice:
```cpp
// freezePage() doesn't hold lock during renderFrames
freezePage() {
  lock;
  checkCache();  // Under lock
  releaseLock;
  
  renderFrames();  // NOT under lock—safe to call freezePage on upstream components
}
```

### 3. **Catches Design Issues**
If a function needs to call another _nolock function, you're forced to think about:
- Why do I need to call this?
- Am I holding the right lock?
- Is my lock scope too large?

This prevents "lock everything" anti-patterns.

### 4. **Enables Proper Refactoring**
When optimizing lock scope, _nolock functions show what can run in parallel:
```cpp
// Original: long operation under lock
calcOutput() {
  lock;
  expensiveOperation();  // 100ms
  unlock;
}

// Refactored: operation outside lock
calcOutput() {
  lock;
  checkPreconditions();  // Quick check
  setupData();           // Setup
  allocateOutput();      // Allocate result structure
  unlock;
  
  // This can run in parallel with other threads
  expensiveOperation();  // Now outside lock!
}
```

## Implementation Guidelines

### 1. **Function Naming**
- Public/external: `functionName()`
- Internal (assumes lock held): `functionName_nolock()`
- Document: "Caller must hold mutex()"

### 2. **Lock Acquisition Pattern**
```cpp
// Public function
void twComponent::freezePage(uint64_t pos) {
  // Lock only for fast operations
  std::shared_ptr<Page> page;
  bool needsRender = false;
  
  {
    std::lock_guard<std::mutex> lock(mutex());
    // Cache check under lock
    page = getCachedPage(pos);
    if (!page) {
      // Allocate and insert under lock to prevent duplicates
      page = allocatePlaceholder(pos);
      needsRender = true;
    }
  } // Lock released
  
  // Can return early without holding lock
  if (!needsRender) return page;
  
  // Call _nolock helper for expensive work (outside lock)
  freezePage_nolock(page);
}

// Private helper
void twComponent::freezePage_nolock(std::shared_ptr<Page> page) {
  // Assume: lock is NOT held (caller released it)
  // Safe: can call renderFrames which might recursively freeze pages
  renderFrames(page);
}
```

### 3. **When Lock is Already Held**
If you're in a context where the lock is guaranteed held (e.g., inside another _nolock function), call _nolock variants directly:

```cpp
void twComponent::invalidateAllPages_nolock() {
  // Assume caller holds lock
  for (auto& page : outputPages_) {
    page->validAspects = 0;  // Modify under assumed lock
    invalidateDependents_nolock();  // Call other _nolock functions
  }
}
```

### 4. **Shared Work Between Lock/Nolock Contexts**
If logic needs to run both with and without the lock, use a helper:

```cpp
// Private helper: does work, assumes caller manages lock
void twComponent::restoreState_impl(const State& snapshot) {
  // Work here (caller manages locking)
  internalState_ = snapshot;
}

// With lock (public API)
void twComponent::restoreState(const State& snapshot) {
  std::lock_guard<std::mutex> lock(mutex());
  restoreState_impl(snapshot);  // Call helper while holding lock
}

// Without lock (called from rendering)
void twComponent::renderPage_nolock(const State& prev) {
  restoreState_impl(prev);  // Call same helper, no lock needed
}
```

## Common Pitfalls

### ❌ Deadlock Risk: Recursive Lock Acquisition
```cpp
// BAD: Tries to lock while already holding the lock
void twComponent::calcOutput() {
  std::lock_guard<std::mutex> lock(mutex());
  
  // This will try to acquire the same lock
  auto view = source_->viewAtRate(48000);  // Deadlock if viewAtRate tries to lock
}
```

**Fix:** Provide _nolock variant or release lock before calling:
```cpp
// GOOD: Release lock before calling potentially-locking function
void twComponent::calcOutput() {
  std::lock_guard<std::mutex> lock(mutex());
  // ... quick work ...
  // Lock released here
  
  auto view = source_->viewAtRate(48000);  // Safe now
}
```

### ❌ Data Race: Accessing Mutable State Without Lock
```cpp
// BAD: _nolock function accesses mutable state without lock
void twComponent::getValue_nolock() {
  // Caller must hold lock, but we can't enforce it
  return cachedValue_;  // Race condition if caller didn't hold lock
}
```

**Fix:** Document requirements clearly and consider assertions:
```cpp
// GOOD: Clear documentation
void twComponent::getValue_nolock() {
  // Caller must hold mutex()
  // Debug assertion for development
  assert(mutex().tryLock() == false);  // Should be unable to acquire (already held)
  return cachedValue_;
}
```

### ❌ Over-Locking: Holding Lock During I/O
```cpp
// BAD: Lock held during disk I/O (blocks other threads for seconds)
void twComponent::loadFile() {
  std::lock_guard<std::mutex> lock(mutex());
  diskFile_.read(data_);  // Slow! Lock held entire time
  computeHash();
}
```

**Fix:** Separate fast and slow operations:
```cpp
// GOOD: I/O outside lock
void twComponent::loadFile() {
  // I/O without lock
  auto data = readDiskFile();
  
  // Quick update under lock
  {
    std::lock_guard<std::mutex> lock(mutex());
    this->data_ = data;
  }
  
  computeHash();  // After lock released
}
```

## Real Examples in Smaragd

### freezePage (Page Cache Rendering)
```cpp
// Public: acquires lock only for cache check
std::shared_ptr<twOutputPage> twComponent::freezePage(...) {
  std::shared_ptr<twOutputPage> page;
  bool needsRendering = false;
  
  {
    std::lock_guard<std::mutex> lock(mutex());
    // Check cache and allocate placeholder under lock
    if (outputPages_.count(startPos)) {
      page = outputPages_[startPos];
    } else {
      page = allocatePlaceholder(startPos);
      needsRendering = true;
    }
  } // Lock released - critical!
  
  if (!needsRendering) return page;
  
  // Rendering outside lock—safe for recursive freezePage on upstream components
  freezePage_nolock(page, ...);
  
  return page;
}

// Private: does actual rendering work
length_t twComponent::freezePage_nolock(std::shared_ptr<twOutputPage> page, ...) {
  // Caller released lock—we can recursively call freezePage on dependencies
  return renderFrames(page->samples.data(), ...);
}
```

### viewAtRate (Resampling Setup)
```cpp
// Public: acquires lock to check/create resampler
twRandomSource* twSampleSource::viewAtRate(int targetRate) {
  if (!loaded_ || targetRate == rate_) {
    return this;  // Fast path, no lock needed
  }
  
  std::lock_guard<std::mutex> lock(mutex());
  return viewAtRate_nolock(targetRate);  // Call helper while holding lock
}

// Private: does setup under held lock
twRandomSource* twSampleSource::viewAtRate_nolock(int targetRate) {
  if (!resampled_ || resampledRate_ != targetRate) {
    resampled_.reset(new twResampledSource(*this, targetRate));
  }
  return resampled_.get();
}
```

## Migration Strategy

When refactoring existing code to use _nolock:

1. **Identify lock-heavy functions** that hold mutex during slow operations
2. **Split into two parts:**
   - Public: minimal work, release before expensive calls
   - _nolock: expensive work, assumes lock not needed
3. **Update call sites:**
   - External callers → call public variant (they don't hold lock)
   - Internal callers that just released lock → call _nolock variant
4. **Add documentation** above _nolock functions: "Caller must hold mutex()" or "Caller must NOT hold mutex()"
5. **Test:** Verify no deadlocks and performance improves

## Summary

The _nolock() pattern trades minimal convenience ("just use recursive_mutex") for major benefits:

- **Explicit:** Lock scope is visible in code
- **Safe:** Deadlock-free by design for recursive operations
- **Optimizable:** Shows which operations can run in parallel
- **Debuggable:** Clear invariants about when locks are held

This pattern is essential for Smaragd's audio rendering pipeline, where components recursively freeze pages for playback—hiding lock scope with recursive_mutex would mask the root cause of timing bugs.
