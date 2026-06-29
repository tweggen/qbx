# Lock-Free Lazy Initialization with std::call_once

## Overview

`std::call_once` + `std::once_flag` provides an elegant solution for thread-safe lazy initialization that avoids common pitfalls of manual lock management:

- **No constructor called under lock** — expensive work happens outside the critical section
- **Race-free initialization** — exactly one construction, even with concurrent calls
- **Multiple cached objects** — one per key (not one slot)
- **No manual lock release** — `call_once` handles synchronization internally
- **Simple, proven pattern** — standard library, audited and well-tested

## Problem: Manual Lock Management Issues

**Naive approach (WRONG):**
```cpp
std::mutex m;
std::map<Key, std::shared_ptr<Object>> cache;

std::shared_ptr<Object> get(const Key& k) {
    std::unique_lock<std::mutex> lock(m);
    
    auto it = cache.find(k);
    if (it != cache.end()) {
        return it->second;  // Found
    }
    
    // Create object WHILE HOLDING LOCK ❌ BLOCKS AUDIO THREAD
    auto obj = std::make_shared<Object>(k);
    cache[k] = obj;
    return obj;
}
```

**Problems:**
1. Constructor runs under mutex → if expensive, blocks all other threads
2. TOCTOU race: constructor might fail, but cache is already updated
3. No deterministic behavior if constructor throws
4. Audio thread blocks on expensive setup work

**Better approach (using call_once):**
```cpp
struct Entry {
    std::once_flag flag;
    std::shared_ptr<Object> obj;
};

std::mutex m;
std::map<Key, Entry> cache;

std::shared_ptr<Object> get(const Key& k) {
    Entry* entry = nullptr;
    
    // Lock only for dictionary access (brief)
    {
        std::lock_guard<std::mutex> lock(m);
        entry = &cache[k];
    }  // Lock released
    
    // Constructor called HERE, outside the lock ✅
    // call_once ensures exactly one execution per (once_flag, callable) pair
    std::call_once(entry->flag, [&] {
        entry->obj = std::make_shared<Object>(k);
    });
    
    return entry->obj;
}
```

**Advantages:**
1. Lock held only for `cache[k]` map lookup (O(log n), microseconds)
2. Constructor (expensive work) runs without lock
3. Concurrent calls serialize at `call_once`, not at mutex
4. Exception-safe: if constructor throws, `once_flag` is still set (won't retry)

## Real-World Example: twSampleSource::viewAtRate()

### The Problem

Original code:
```cpp
// Issue 1: Read properties without lock (TOCTOU)
if (!loaded_ || targetRate == rate_) return this;

// Issue 2: Hold lock during expensive constructor
std::lock_guard<std::mutex> lock(mutex_);
resampled_.reset(new twResampledSource(*this, targetRate));  // BLOCKS!

// Issue 3: Only one cached view (overwrites previous)
resampledRate_ = targetRate;  // Lose previous rate's view
```

**Consequences:**
- Audio thread blocks during resampler creation (~10-100ms)
- UI thread blocks waiting for audio thread
- Only one resampled view cached; calling with rates A, B, A rebuilds each time

### The Solution

```cpp
// Cache entry with call_once semantics
struct ResampledEntry {
    std::once_flag flag;
    std::shared_ptr<twResampledSource> obj;
};

std::map<int, ResampledEntry> resampledCache_;  // One per rate!

twRandomSource *twSampleSource::viewAtRate(int targetRate) const {
    // Fast path: native rate (no lock)
    if (!loaded_ || targetRate == rate_) return this;
    
    // Access cache under lock (brief)
    ResampledEntry* entry = nullptr;
    {
        std::lock_guard<std::mutex> lock(resampledMutex_);
        auto& e = resampledCache_[targetRate];
        entry = &e;
    }  // Lock released immediately
    
    // Call constructor OUTSIDE lock via call_once
    // - Thread A calls with rate 48000: creates resampler, sets flag
    // - Thread B calls with rate 48000: waits at call_once, gets same object
    // - Thread C calls with rate 44100: creates different resampler
    std::call_once(entry->flag, [&] {
        entry->obj = std::make_shared<twResampledSource>(*this, targetRate);
    });
    
    return entry->obj.get();
}
```

**Benefits:**
1. ✅ Fast path (native rate) requires no lock at all
2. ✅ Lock held only for map access (~1 microsecond)
3. ✅ Resampler construction (expensive) runs without lock
4. ✅ Multiple rates cached independently
5. ✅ Audio thread never blocks on constructor
6. ✅ Exception-safe: `call_once` won't retry on constructor failure

## How std::call_once Works

```cpp
std::once_flag flag;
std::mutex m;
int value = 0;

// Thread 1:
std::call_once(flag, [&] {
    std::this_thread::sleep_for(std::chrono::seconds(2));  // Expensive work
    value = 1;
});
// Returns immediately after setting value=1

// Thread 2 (calls simultaneously):
std::call_once(flag, [&] {
    value = 2;  // THIS LAMBDA NEVER RUNS
});
// Blocks until Thread 1's lambda completes, then returns
// value is 1 (not 2)

// Thread 3 (calls after Thread 1):
std::call_once(flag, [&] {
    value = 3;  // THIS LAMBDA NEVER RUNS
});
// Returns immediately
// value is still 1

// Summary:
// - Exactly one execution, even with 3 concurrent callers
// - No explicit lock in user code
// - Callers block only until the one execution completes (not a spinlock—uses futex/event)
```

## Key Properties

| Property | Behavior |
|----------|----------|
| **Thread-safety** | Guaranteed; uses atomic operations internally |
| **Performance** | First call: ~1μs overhead; subsequent: lock-free |
| **Exception safety** | If callable throws, flag is still set (won't retry) |
| **Lock-free** | Yes; uses futex/event, not mutex |
| **Deterministic** | Exactly one execution, always |
| **Multiple keys** | Yes; use std::map<Key, Entry> for independent flags |

## Anti-Pattern: Double-Checked Locking (Don't Do This)

Programmers often try to optimize by checking before locking:

```cpp
// WRONG: Memory visibility issue
std::shared_ptr<Object> obj = nullptr;

std::shared_ptr<Object> get() {
    if (obj) return obj;  // ❌ Unsynchronized read
    
    std::lock_guard<std::mutex> lock(m);
    if (obj) return obj;  // Double-check (correct)
    
    obj = std::make_shared<Object>();
    return obj;
}
```

**Problem:** First check (without lock) is a data race. Result is undefined behavior.

**Correct double-checked locking (complex, error-prone):**
```cpp
std::atomic<std::shared_ptr<Object>> obj;

std::shared_ptr<Object> get() {
    auto local = obj.load(std::memory_order_acquire);
    if (local) return local;  // Acquire synchronizes with Release
    
    std::lock_guard<std::mutex> lock(m);
    local = obj.load(std::memory_order_relaxed);
    if (local) return local;
    
    local = std::make_shared<Object>();
    obj.store(local, std::memory_order_release);
    return local;
}
```

**Better:** Just use `call_once`. It's simpler, proven, and doesn't require careful memory ordering.

## When to Use call_once

### Good Fit:
- Lazy initialization of expensive objects (resampler, filter kernel, format converter)
- Singleton-like patterns (one object per key)
- One-time setup (GPU shader compilation, codec initialization)
- Caches where miss cost is high (worth the initialization overhead)

### Not a Fit:
- Frequently contended locks (use spin-lock or lock-free structure)
- Objects that need to be recreated (call_once can't be reset)
- Very short initialization (overhead not worth it)

## Summary

`std::call_once` is the **superior choice for lazy initialization** because it:

1. ✅ Separates cache lookup (under lock) from object construction (outside lock)
2. ✅ Eliminates manual lock release complexity
3. ✅ Guarantees exactly one construction, even with concurrent calls
4. ✅ Supports multiple independent objects (one per key)
5. ✅ Is part of the standard library (portable, audited)
6. ✅ Has near-zero overhead after initialization
7. ✅ Makes audio code happy (no blocking on construction)

**Use it for any thread-safe lazy-init pattern in realtime audio code.**
