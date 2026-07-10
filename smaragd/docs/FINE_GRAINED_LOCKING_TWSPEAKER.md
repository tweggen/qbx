# twSpeaker: Fine-Grained Locking Strategy

## Problem Statement

Previously, `twSpeaker` held a single broad `outputMutex_` across expensive I/O operations:
- Device opening (`backend_->openDevice()`)
- Engine creation (`AudioEngine` constructor)
- Backend shutdown (`backend_->stopOutput()` blocks on callback thread)
- Device closing (`backend_->closeDevice()`)

This caused **UI thread freezes** of 100-500ms when calling `stopOutput()`, blocking concurrent operations like `setCycle()` and reducing responsiveness.

## Solution: Fine-Grained Locking

We refactored to use **three independent locks** for different concerns:

| Lock | Purpose | Scope | Max Hold Time |
|------|---------|-------|---------------|
| `stateMutex_` (inherited) | `outputState_` transitions | State check-and-set | ~1 µs |
| `engineMutex_` (new) | `audioEngine_` lifecycle | Creation/destruction | ~1 µs |
| `taskMutex_` (new) | `bufferingTask_` lifecycle | Thread creation/joining | Variable |

Each lock is released **before expensive I/O**, preventing blocking of other threads.

## Locking Strategy by Operation

### startOutput()

```
Phase 1: Check state (stateMutex_)       [~1 µs]
Phase 2: Open device, negotiate rates    [No lock - ~100 ms I/O]
Phase 3: Create engine (engineMutex_)    [~1 µs]
Phase 4: Configure engine                [No lock - ~10 ms]
Phase 5: Register callback               [No lock - ~1 µs]
Phase 6: Transition to BUFFERING         [stateMutex_ - ~1 µs]
Phase 7: Spawn monitor task (taskMutex_) [~1 µs]
```

**Total time without holding state lock:** ~110 ms for expensive I/O ✅

### stopOutput()

```
Phase 1: Check state (stateMutex_)       [~1 µs]
Phase 2: Stop buffering task (taskMutex_)[Variable, may join]
Phase 3: Stop backend output             [No lock - blocks on callback]
Phase 4: Close device                    [No lock - ~50 ms I/O]
Phase 5: Destroy engine (engineMutex_)   [~1 µs]
Phase 6: Final state transition          [stateMutex_ - ~1 µs]
```

**Total time without holding state lock:** ~100+ ms ✅

### setCycle()

```
Atomically store loop parameters         [No lock, atomic ops]
Acquire engineMutex_                     [~1 µs]
Call audioEngine_->setLoopBoundaries()   [~1 µs]
Release engineMutex_
```

**Total hold time:** ~2 µs (compared to previous 100+ ms if stopOutput was running) ✅

## Thread Safety Analysis

### audioEngine_ Lifecycle

- `startOutput()` creates `audioEngine_` under `engineMutex_`
- `monitorReadaheadBuffer()` accesses under `engineMutex_`
- `setCycle()` accesses under `engineMutex_`
- `stopOutput()` destroys under `engineMutex_`
- Render callback captures via shared_ptr (lock-free)

**Result:** No use-after-free; render callback holds reference even if engine reset.

### bufferingTask_ Lifecycle

- `startOutput()` creates under `taskMutex_`
- `stopOutput()` joins under `taskMutex_`
- No concurrent access possible; interleaving is serialized

**Result:** No double-join or use-after-free on std::thread.

### outputState_ Transitions

- All state transitions guarded by `stateMutex_` (inherited from twComponent)
- Atomic reads for lock-free status checks
- Prevents concurrent entry into critical sections

**Result:** State machine integrity maintained across threads.

## Benefits

1. **No UI freezes:** Device I/O doesn't block state mutex
2. **Faster setCycle():** No contention with active stopOutput()
3. **Read-ahead unburdened:** `monitorReadaheadBuffer()` doesn't block on expensive ops
4. **Clear separation of concerns:** Each lock has one responsibility
5. **Inherits twComponent pattern:** Uses `stateMutex_` as other components do

## Trade-offs

- **Slightly more complex:** Three locks instead of one (but each is brief and clear)
- **Requires careful interleaving:** Must ensure operations outside lock aren't state-dependent
  - **Mitigated by:** Atomic state flags; each phase is logically independent

## Testing Checklist

- [ ] Start playback; UI remains responsive
- [ ] Stop playback while in BUFFERING state; clean shutdown (no dangling threads)
- [ ] Stop playback while in PLAYING state; audio stops smoothly
- [ ] Call `setCycle()` during playback; no delays
- [ ] Delete track during playback; UI doesn't freeze
- [ ] Rapid play/stop toggling; no crashes or hangs
- [ ] Verify no mutex deadlocks in all scenarios

## Future Improvements

1. **Async backend shutdown:** Spawn cleanup task so `stopOutput()` returns immediately
2. **Condition variables:** Replace polling in `monitorReadaheadBuffer()` with efficient CV wait
3. **Lock-free state:** Consider atomic state machines if contention becomes problematic

## References

- `tw303a/include/twcomponent.h` — `stateMutex_` definition and usage pattern
- `tw303a/include/twspeaker.h` — Class definition with three locks
- `tw303a/src/twspeaker.cc` — Implementation with fine-grained locking
