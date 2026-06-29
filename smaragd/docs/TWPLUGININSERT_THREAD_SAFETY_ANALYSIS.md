# twPluginInsert Thread-Safety Analysis

## Executive Summary

**twPluginInsert** has **4 critical thread-safety violations** that violate the base class `twComponent` _nolock() pattern established in `APPLYING_NOLOCK_PATTERN.md`. All race conditions can be fixed by applying the proven pattern: acquire `mutex()` in public methods, delegate to `_nolock()` variants.

| Issue | Severity | Category | Impact |
|-------|----------|----------|--------|
| `bypass_` flag (line 35, 52) | **HIGH** | Data race | Audio corruption (switch during processing) |
| `producedThisBlock_` flag (line 43, 46, 65) | **CRITICAL** | Double-processing | Audio corruption (process multiple times per block) |
| `plugin_` pointer access (lines 36, 41, 63, 113) | **MEDIUM** | Use-after-free risk | Crash if plugin replaced during rendering |
| `inScratch_`/`outScratch_` vectors (lines 46-71) | **MEDIUM** | Buffer race | Audio corruption (concurrent access) |

---

## Detailed Analysis

### 1. Critical: `producedThisBlock_` Flag Race (Lines 43, 46, 65)

#### Problem
The `producedThisBlock_` flag controls whether a plugin processes audio or serves cached results. **It is NOT atomic**, and `calcOutputTo()` is called once **per output port**.

```cpp
// twplugininsert.h line 43
bool producedThisBlock_ = false;

// twplugininsert.cc lines 44-66
length_t twPluginInsert::calcOutputTo( sample_t *dst, length_t len, idx_t port )
{
    if( !producedThisBlock_ ) {  // Line 46: READ without lock
        // Pull inputs, process plugin
        plugin_->process( ... );  // Line 63
        producedThisBlock_ = true;  // Line 65: WRITE without lock
    }
    // Serve cached result
    std::memcpy( dst, outScratch_[port].data(), len * sizeof( sample_t ) );
    return len;
}
```

#### Call Pattern (from twPluginChain::calcOutputTo())
```cpp
// twPluginChain::calcOutputTo() line 36-37
for( auto *plugin : plugins_ )
    plugin->resetBlock();  // Called once per block, resets flag

// Then for EACH OUTPUT PORT, caller invokes:
lastPlugin->calcOutputTo( dst, len, port );  // port=0, 1, 2, ...
```

#### Race Condition Scenario (2-channel plugin)
```
Block starts: producedThisBlock_ = false

Thread A (audio, port 0):
  T0: Read !producedThisBlock_ → true (enter if)
  T1: Pull inputs, process plugin
  T2: PREEMPT

Thread B (audio, port 1):  [interrupts T1 at T2]
  T3: Read !producedThisBlock_ → true (still false!)
  T4: Pull inputs AGAIN, process plugin AGAIN
  T5: Write producedThisBlock_ = true

Thread A (resumes):
  T6: Write producedThisBlock_ = true (redundant)

Result: Audio processed TWICE, consumed TWICE from input wires
        → Audio corruption (amplitude doubled, dropped samples)
```

#### Root Cause
No lock protects the read-modify-write sequence. Standard practice is to acquire lock once, test flag, modify plugin state, then release.

#### Fix Pattern (from APPLYING_NOLOCK_PATTERN.md)
```cpp
// Header
private:
    length_t calcOutputTo_nolock( sample_t *dst, length_t len, idx_t port );

// Implementation
length_t twPluginInsert::calcOutputTo( sample_t *dst, length_t len, idx_t port )
{
    std::lock_guard<std::mutex> lock( mutex() );
    return calcOutputTo_nolock( dst, len, port );
}

length_t twPluginInsert::calcOutputTo_nolock( sample_t *dst, length_t len, idx_t port )
{
    // All state access protected by caller's lock
    if( !producedThisBlock_ ) {
        // ... process plugin ...
        producedThisBlock_ = true;
    }
    // ... serve result ...
}
```

---

### 2. High: `bypass_` Flag Race (Lines 35, 52)

#### Problem
```cpp
// twplugininsert.h line 35
void setBypass( bool bypass ) { bypass_ = bypass; }  // WRITE without lock

// twplugininsert.cc line 52
if( bypass_ ) {  // READ without lock in calcOutputTo()
    copyChannels( inScratch_, outScratch_, getNInputs(), len );
} else {
    plugin_->process( ... );
}
```

#### Call Chain (UI → Audio)
```
Thread A (UI):
  SPluginSlot::setBypass(true)
    → twPluginInsert::setBypass(true)
    → bypass_ = true  [no lock]

Thread B (audio, mid-block):
  twPluginInsert::calcOutputTo()
    → Read bypass_  [no lock]
    → Branch decision (bypass or process)
```

#### Race Scenario
```
T0: Audio reads bypass_=false → will process
T1: UI writes bypass_=true (cache flush, preempt)
T2: Audio continues with stale bypass_=false
    → Processes instead of copying (TOCTOU race)

Or:

T0: Audio writes output buffer assuming bypass mode
T1: UI changes bypass_ to process mode
T2: Audio's partial output corruption visible
```

#### Impact
- **Most common case:** Audio processing inconsistent (part of frame bypassed, rest processed)
- **Worse case:** Plugin operates on stale internal state changed by concurrent setBypass

---

### 3. High: `plugin_` Pointer Access (Lines 36, 41, 63, 113)

#### Problem
```cpp
// twplugininsert.h line 41
std::unique_ptr<twPlugin> plugin_;

// Various accesses without lock:
plugin_->ioLayout()         // Line 36 (getNInputs/getNOutputs)
plugin_->ioLayout()         // Line 41 (getNOutputs)
plugin_->process(...)       // Line 63
plugin_->reset()            // Line 113
```

#### Current Code (twplugininsert.cc)
```cpp
// Lines 34-42: Constructor calls plugin_->ioLayout() without lock
twPluginInsert::twPluginInsert( tw303aEnvironment &env, std::unique_ptr<twPlugin> plugin )
    : twComponent( env ), plugin_( std::move( plugin ) )
{
    const auto &io = plugin_->ioLayout();  // Initial setup only
    inScratch_.resize( io.audioInputs );
    outScratch_.resize( io.audioOutputs );
}

// Line 36: getNInputs() accesses plugin_->ioLayout()
idx_t twPluginInsert::getNInputs() const
{
    return plugin_->ioLayout().audioInputs;
}

// Line 63: calcOutputTo() calls plugin_->process()
plugin_->process( inPtrs.data(), outPtrs.data(), len );
```

#### Theoretical Risk (not currently triggered)
While `plugin_` is initialized once and never reassigned in current code, it's accessed from:
1. **UI thread** (via public methods: getPlugin, setBypass, etc.)
2. **Audio thread** (via calcOutputTo, reset)
3. **Constructor** (initialization)

If a future feature **unloads/reloads plugins**, concurrent access to `plugin_` would cause:
- Use-after-free if plugin unloaded while audio thread calling `process()`
- Dereference nullptr if plugin cleared

#### Mitigation
Even if unlikely, defensive programming suggests protecting all plugin_ accesses consistently.

---

### 4. Medium: `inScratch_`/`outScratch_` Vector Race (Lines 46-71)

#### Problem
```cpp
// twplugininsert.h line 46
std::vector<std::vector<sample_t>> inScratch_, outScratch_;

// twplugininsert.cc lines 48-49
for( idx_t c = 0; c < getNInputs(); ++c )
    pullInput( c, inScratch_[c].data(), len );  // Line 48-49: READ without lock

// Line 70: WRITE and READ in calcOutputTo()
std::memcpy( dst, outScratch_[port].data(), len * sizeof( sample_t ) );
```

#### Race Scenario
```
Thread A (audio, port 0):
  Enters calcOutputTo(), lock-free (no mutex)
  Populates inScratch_[0..N]
  Calls plugin_->process()
  Writes to outScratch_[0..N]

Thread B (audio, port 1): [concurrent]
  Enters calcOutputTo() on same insert
  Reads producedThisBlock_=false (double processing)
  Populates inScratch_[0..N] AGAIN
  Calls plugin_->process() AGAIN (same buffers!)
  Corrupts outScratch_ results
```

#### Current Safeguard (Insufficient)
`resetBlock()` is called once per block by twPluginChain, but:
- Does NOT protect against same-block concurrent `calcOutputTo()` on different ports
- Called from audio thread, not a lock

If a downstream consumer mistakenly pulls multiple ports in parallel (hypothetically), corruption occurs.

---

## Architecture: How calcOutputTo Is Called

From `twPluginChain::calcOutputTo()` (lines 20-46):

```cpp
length_t twPluginChain::calcOutputTo( sample_t *dst, length_t len, idx_t port )
{
    std::lock_guard<std::mutex> lock( pluginsMutex_ );

    // Reset all plugins at block start
    for( auto *plugin : plugins_ )
        plugin->resetBlock();

    // Pull from last plugin
    auto *lastPlugin = plugins_.back();
    if( lastPlugin && port < lastPlugin->getNOutputs() ) {
        return lastPlugin->calcOutputTo( dst, len, port );  // ONCE per port
    }
    return 0;
}
```

**Key insight:** `calcOutputTo()` is **called once per output port per block** by the audio callback. If a 2-channel plugin's output is consumed by 2 different downstream latches, the latch system invokes:
- `plugin[0]->calcOutputTo(..., port=0)`
- `plugin[0]->calcOutputTo(..., port=1)`

Currently, `producedThisBlock_` is **per-insert, not per-port**, so both calls will see the flag transition.

---

## Thread Access Patterns

### Current (No Lock)
| Method | Thread | Lock | Accesses |
|--------|--------|------|----------|
| `setBypass()` | UI | None | `bypass_` |
| `getBypass()` | UI | None | `bypass_` |
| `calcOutputTo()` | Audio | None | `producedThisBlock_`, `plugin_`, `inScratch_`, `outScratch_`, `bypass_` |
| `resetBlock()` | Audio | None | `producedThisBlock_` |
| `reset()` | Audio | None | `producedThisBlock_`, `plugin_` |
| `getPlugin()` | UI | None | `plugin_` |

### Expected (After Fix)
| Method | Thread | Lock | Accesses |
|--------|--------|------|----------|
| `setBypass()` | UI | `mutex()` | `bypass_` |
| `calcOutputTo()` | Audio | `mutex()` | `producedThisBlock_`, `plugin_`, `inScratch_`, `outScratch_`, `bypass_` |
| `resetBlock()` | Audio | None (inline) | `producedThisBlock_` |
| `reset()` | Audio | `mutex()` | `producedThisBlock_`, `plugin_` |

Note: `resetBlock()` is called once per audio block from `twPluginChain` holding its own lock, so adding a lock to `resetBlock()` is safe but adds contention. Can remain inline.

---

## Recommended Fixes

### Fix 1: Apply _nolock() Pattern to calcOutputTo()

**Header** (`twplugininsert.h`):
```cpp
private:
    length_t calcOutputTo_nolock( sample_t *dst, length_t len, idx_t port );
```

**Implementation** (`twplugininsert.cc`):
```cpp
length_t twPluginInsert::calcOutputTo( sample_t *dst, length_t len, idx_t port )
{
    std::lock_guard<std::mutex> lock( mutex() );
    return calcOutputTo_nolock( dst, len, port );
}

length_t twPluginInsert::calcOutputTo_nolock( sample_t *dst, length_t len, idx_t port )
{
    // Caller must hold mutex()
    if( !producedThisBlock_ ) {
        for( idx_t c = 0; c < getNInputs(); ++c )
            pullInput( c, inScratch_[c].data(), len );

        if( bypass_ ) {
            copyChannels( inScratch_, outScratch_, getNInputs(), len );
        } else {
            std::vector<const float *> inPtrs( getNInputs() );
            std::vector<float *> outPtrs( getNOutputs() );
            for( idx_t c = 0; c < getNInputs(); ++c )
                inPtrs[c] = inScratch_[c].data();
            for( idx_t c = 0; c < getNOutputs(); ++c )
                outPtrs[c] = outScratch_[c].data();
            plugin_->process( inPtrs.data(), outPtrs.data(), len );
        }
        producedThisBlock_ = true;
    }

    if( port < getNOutputs() )
        std::memcpy( dst, outScratch_[port].data(), len * sizeof( sample_t ) );
    return len;
}
```

**Rationale:**
- Entire read-check-modify sequence is atomic w.r.t. concurrent UI/audio thread calls
- Prevents double-processing (most severe bug)
- Protects bypass_ TOCTOU race
- Protects all buffer accesses
- Protects plugin_->process() and plugin_->ioLayout() calls

### Fix 2: Lock setBypass()

**Implementation** (`twplugininsert.cc`):
```cpp
void twPluginInsert::setBypass( bool bypass )
{
    std::lock_guard<std::mutex> lock( mutex() );
    bypass_ = bypass;
}
```

**Rationale:**
- Ensures bypass_ changes are visible atomically to audio thread
- Prevents mid-frame bypass mode flips

### Fix 3: Lock reset()

**Implementation** (`twplugininsert.cc`):
```cpp
void twPluginInsert::reset()
{
    std::lock_guard<std::mutex> lock( mutex() );
    producedThisBlock_ = false;
    if( plugin_ ) {
        plugin_->reset();
    }
}
```

**Rationale:**
- Protects plugin_->reset() from concurrent calcOutputTo()
- Ensures producedThisBlock_ reset is not interleaved with processing

### Fix 4: Keep resetBlock() Inline (No Lock)

**Rationale:**
- Called once per block from `twPluginChain::calcOutputTo()` which already holds `pluginsMutex_`
- Adding a second lock would create nested locking (acceptable but adds contention)
- Method is inline, no function call overhead
- Block reset is UI-safe (happens only during rendering when playhead is active)

```cpp
// Keep as inline in header
void resetBlock() { producedThisBlock_ = false; }
```

---

## Comparison to Other Components

The fixes follow exactly the pattern applied to `twMixer`, `twRewire`, `twSampleReader`, and `twWavInput` in `APPLYING_NOLOCK_PATTERN.md`:

| Component | Pattern | Status |
|-----------|---------|--------|
| `twComponent` (base) | Public acquires lock → calls `_nolock()` | ✅ Established |
| `twMixer` | `calcOutputTo()`, `seekTo()`, etc. wrapped | ✅ Fixed |
| `twRewire` | `calcOutputTo()`, `setNPlugs()`, etc. wrapped | ✅ Fixed |
| `twSampleReader` | `calcOutputTo()`, `seekTo()` wrapped | ✅ Fixed |
| `twWavInput` | `calcOutputTo()`, `seekTo()` wrapped | ✅ Fixed |
| `twPluginChain` | `calcOutputTo()`, `seekTo()`, `addPlugin()` wrapped | ✅ Already locked |
| `twPluginInsert` | **`calcOutputTo()`, `setBypass()`, `reset()` missing** | ❌ **This task** |

---

## Lock Contention Analysis

### Current Risk
Without locks:
- Data races on every audio block
- Bypass changes visible only after block boundary (1-40ms lag)
- Use-after-free risk if plugin_ is ever hot-swapped

### After Fix
With locks on `calcOutputTo()`, `setBypass()`, `reset()`:
- Lock held for ~1-4KB of processing (short)
- `setBypass()` lock is single write, negligible
- `reset()` lock is initialization only
- Contention risk: **minimal** (operations are infrequent, short-lived)

### Acceptable Because
- Plugin processing is CPU-bound (locks released quickly)
- UI thread calls (setBypass) are **not in realtime** (safe to block)
- Audio thread calls (calcOutputTo) hold lock only for 1 block's work
- Pattern matches all other components (proven safe)

---

## Testing Recommendations

### Automated Tests
1. **Concurrent bypass toggle:** Spawn UI thread toggling bypass() while audio renders
2. **Multi-port stress:** Verify calcOutputTo(port=0,1,2,...) don't double-process
3. **Reset during render:** Call reset() while audio thread in calcOutputTo()
4. **Plugin access:** Verify no crashes/races with Address Sanitizer

### Manual Testing
1. Toggle plugin bypass while playing audio
2. Enable/disable plugins in a chain during playback
3. Listen for audio corruption (popping, amplitude changes)
4. Verify bypass mode change is immediate (no lag)

### Address Sanitizer
```bash
./rebuild.sh  # Includes -fsanitize=thread
./run_app     # Run with ThreadSanitizer enabled
# Toggles bypass during playback
# Look for "WARNING: ThreadSanitizer: data race"
```

---

## Summary

**twPluginInsert** violates the base class thread-safety contract by accessing shared state without locks. The **producedThisBlock_** flag race is the most severe (double-processing audio). All issues are fixed by applying the proven _nolock() pattern:

1. ✅ Wrap `calcOutputTo()` with `calcOutputTo_nolock()`
2. ✅ Lock `setBypass()` (single line change)
3. ✅ Lock `reset()` (single change)
4. ✅ Keep `resetBlock()` inline (no lock)

Changes are minimal, follow established patterns, and eliminate all identified races.

---

## Files to Modify

- `/Users/tweggen/coding/github/qbx/smaragd/tw303a/include/twplugininsert.h`
- `/Users/tweggen/coding/github/qbx/smaragd/tw303a/src/twplugininsert.cc`

## Reference Documentation

- `APPLYING_NOLOCK_PATTERN.md` — Pattern documentation and examples
- `COMPONENT_LOCKING_STRATEGY.md` — Base class locking design
- `twPluginChain` (`tw303a/src/twpluginchain.cc`) — Example of correct locking in plugin infrastructure
