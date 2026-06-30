# Component Migration Guide: Adding New DSP Components

**Audience:** Developers extending Smaragd with new audio processing components  
**Prerequisites:** Understanding of twComponent architecture (see `ARCHITECTURE.md` and `COMPONENTS.md`)  
**Goal:** Provide step-by-step process for implementing new components using type-safe IOVector interface

---

## Overview: The Two-Interface Pattern

All new components implement **both interfaces** during Phase 3+ transition:

```cpp
// NEW: Type-safe, bounds-checked (use this for new code)
virtual length_t calcOutputTo(IOVector& dest, idx_t idx) override;

// LEGACY: Raw-pointer interface (kept for backward compatibility)
virtual length_t calcOutputTo(sample_t *pDest, length_t length, idx_t idx) override;
```

**Why both?**
- Allows gradual migration (new components use IOVector)
- Doesn't break existing consumers (raw-pointer path still works)
- Default implementations can wrap the other (no code duplication)

---

## Decision Tree: Which Pattern?

```
Is this a new component?
├─ YES (add new component)
│  └─ Use IOVector-first pattern (below)
│
├─ Existing component (migrate from raw-pointer)
│  └─ Add IOVector override (delegates to raw-pointer if needed)
│
└─ Existing consumer (e.g., AudioEngine)
   └─ Can call either interface; prefer IOVector if available
```

---

## Step-by-Step: Adding a New Component

### Step 1: Create Header File

**File:** `tw303a/include/twexample.h`

```cpp
#ifndef _TW_EXAMPLE_H_
#define _TW_EXAMPLE_H_

#include "twcomponent.h"

class twExample : public twComponent {
public:
    twExample(tw303aEnvironment &env);
    virtual ~twExample();

    // Implement required interface methods
    virtual idx_t getNInputs() const override;
    virtual idx_t getNOutputs() const override;
    virtual const char *getInputName(idx_t idx) const override;
    virtual const char *getOutputName(idx_t idx) const override;

    virtual void createOutputLatches() override;
    virtual void init() override;
    virtual void reset() override;

    // Phase 3: Type-safe interface (preferred for new components)
    virtual length_t calcOutputTo(IOVector& dest, idx_t idx) override;

    // Legacy: Raw-pointer interface (can delegate to IOVector version)
    virtual length_t calcOutputTo(sample_t *pDest, length_t length, idx_t idx) override;

private:
    // Component-specific state
    float gainDb_ = 0.0f;

    // Helper for _nolock implementation
    length_t calcOutputTo_nolock(sample_t *pDest, length_t length, idx_t idx);
};

#endif
```

---

### Step 2: Implement Source File

**File:** `tw303a/src/twexample.cc`

#### 2a. Basic Setup

```cpp
#include "twexample.h"
#include "tw303aenv.h"
#include "io_vector.h"
#include <cstring>

twExample::twExample(tw303aEnvironment &env)
    : twComponent(env),
      gainDb_(0.0f)
{
}

twExample::~twExample()
{
}

void twExample::createOutputLatches()
{
    // One latch per output
    pOutputLatches[0] = new twStreamingLatch(*this, 0, 0);
}

void twExample::init()
{
    twComponent::init();
    // Any one-time initialization
}

void twExample::reset()
{
    std::lock_guard<std::mutex> lock(mutex());
    // Reset internal state (e.g., counters, accumulator positions)
}

idx_t twExample::getNInputs() const { return 1; }
idx_t twExample::getNOutputs() const { return 1; }
const char *twExample::getInputName(idx_t) const { return "Audio input"; }
const char *twExample::getOutputName(idx_t) const { return "Audio output"; }
```

#### 2b. Choose Pattern Based on Component Type

---

### Pattern A: Simple Stateless Processor

**Example:** Gain adjustment, simple distortion, constant output

```cpp
// IOVector version (type-safe, preferred)
length_t twExample::calcOutputTo(IOVector& dest, idx_t outChannel) override
{
    // Read input
    sample_t *inBuffer = (sample_t *)alloca(dest.length() * sizeof(sample_t));
    readInput(0, inBuffer, dest.length(), outChannel);

    // Apply processing
    double factor = pow(10.0, gainDb_ / 20.0);
    for (size_t i = 0; i < dest.length(); i++) {
        inBuffer[i] *= (sample_t)factor;
    }

    // Write to IOVector (bounds-safe)
    return dest.copyFrom(IOVector::CreateFromBuffer(inBuffer, dest.length()), 0, dest.length());
}

// Legacy version (wraps IOVector)
length_t twExample::calcOutputTo(sample_t *pDest, length_t length, idx_t outChannel) override
{
    std::lock_guard<std::mutex> lock(mutex());
    return calcOutputTo_nolock(pDest, length, outChannel);
}

length_t twExample::calcOutputTo_nolock(sample_t *pDest, length_t length, idx_t outChannel)
{
    // Can either:
    // Option 1: Duplicate logic here
    // Option 2: Create wrapper IOVector and call IOVector version
    
    // Option 2 (cleaner):
    IOVector wrapped(std::make_shared<twOutputPage>(), 0, length);
    memcpy(wrapped.samples.data(), pDest, length * sizeof(sample_t));
    length_t result = calcOutputTo(wrapped, outChannel);
    memcpy(pDest, wrapped.samples.data(), result * sizeof(sample_t));
    return result;
}
```

---

### Pattern B: Multi-Input Processor (DSP Filter)

**Example:** Moog filter, mixer, convolver

```cpp
// IOVector version
length_t twExample::calcOutputTo(IOVector& dest, idx_t outChannel) override
{
    // Stack-allocate temp buffers for inputs
    sample_t *audio = (sample_t *)alloca(dest.length() * sizeof(sample_t));
    sample_t *control = (sample_t *)alloca(dest.length() * sizeof(sample_t));

    // Read inputs
    readInput(0, audio, dest.length(), outChannel);      // Audio input
    readInput(1, control, dest.length(), outChannel);    // Control input

    // Apply DSP
    applyFilter(audio, control, dest.length());

    // Write to IOVector
    return dest.copyFrom(IOVector::CreateFromBuffer(audio, dest.length()), 0, dest.length());
}

// Helper: Apply DSP logic (not thread-safe, called under lock)
void twExample::applyFilter(sample_t *audio, sample_t *control, length_t len)
{
    for (size_t i = 0; i < len; i++) {
        // Example: simple gain based on control input
        audio[i] *= (control[i] > 0.5f) ? 1.0f : 0.5f;
    }
}
```

---

### Pattern C: Reader with Position Tracking

**Example:** Sample reader, loop reader, position-aware generator

```cpp
private:
    offset_t playPosition_ = 0;

// IOVector version
length_t twExample::calcOutputTo(IOVector& dest, idx_t outChannel) override
{
    std::lock_guard<std::mutex> lock(mutex());

    sample_t *buffer = (sample_t *)alloca(dest.length() * sizeof(sample_t));

    // Read from source at current position
    source_.read(playPosition_, buffer, dest.length(), outChannel);
    
    // Advance position
    playPosition_ += (offset_t)dest.length();

    return dest.copyFrom(IOVector::CreateFromBuffer(buffer, dest.length()), 0, dest.length());
}

// Internal state snapshot (for sequential rendering)
virtual std::any captureInternalState() const override
{
    struct State {
        offset_t position;
    };
    return std::any(State{playPosition_});
}

virtual void restoreInternalState(const std::any& state) override
{
    try {
        auto s = std::any_cast<const struct State&>(state);
        playPosition_ = s.position;
    } catch (const std::bad_any_cast&) {
        fprintf(stderr, "State format mismatch\n");
    }
}
```

---

### Pattern D: Stateful DSP (Filter with Internal State)

**Example:** Resonant filter, reverb, delay

```cpp
private:
    struct FilterState {
        float pole[4];  // 4-pole filter state
    };
    FilterState state_{};

// IOVector version
length_t twExample::calcOutputTo(IOVector& dest, idx_t outChannel) override
{
    std::lock_guard<std::mutex> lock(mutex());

    sample_t *audio = (sample_t *)alloca(dest.length() * sizeof(sample_t));
    readInput(0, audio, dest.length(), outChannel);

    // Apply filter (modifies state_)
    applyFilterDSP(audio, dest.length());

    return dest.copyFrom(IOVector::CreateFromBuffer(audio, dest.length()), 0, dest.length());
}

// Internal state snapshot
virtual std::any captureInternalState() const override
{
    std::lock_guard<std::mutex> lock(mutex());
    return std::any(state_);
}

virtual void restoreInternalState(const std::any& state) override
{
    std::lock_guard<std::mutex> lock(mutex());
    try {
        state_ = std::any_cast<const FilterState&>(state);
    } catch (const std::bad_any_cast&) {
        fprintf(stderr, "State mismatch\n");
    }
}

// DSP implementation
void twExample::applyFilterDSP(sample_t *audio, length_t len)
{
    for (size_t i = 0; i < len; i++) {
        // Example: simple single-pole lowpass
        state_.pole[0] += 0.1f * (audio[i] - state_.pole[0]);
        audio[i] = state_.pole[0];
    }
}
```

---

### Pattern E: Conditional Routing

**Example:** Mute switch, bypass logic, conditional mixing

```cpp
// IOVector version
length_t twExample::calcOutputTo(IOVector& dest, idx_t outChannel) override
{
    // Check condition
    if (shouldMute_) {
        return dest.fillSilence(0, dest.length());
    }

    // Or pass through input
    if (!isConnected()) {
        return dest.fillSilence(0, dest.length());
    }

    // Or forward to child component
    return childComponent_->calcOutputTo(dest, outChannel);
}
```

---

### Step 3: Add to CMakeLists.txt

**File:** `tw303a/CMakeLists.txt`

#### Add header:
```cmake
set(TW303A_HEADERS
    # ... existing headers ...
    include/twexample.h
    # ... more headers ...
)
```

#### Add source:
```cmake
set(TW303A_SOURCES
    # ... existing sources ...
    src/twexample.cc
    # ... more sources ...
)
```

---

### Step 4: Wire into Component Registry (if needed)

Some components need factory registration. Check if your component needs it:

**File:** `tw303a/src/tw303a.cc` (if component needs dynamic creation)

```cpp
// Register factory
twComponent* createExample(tw303aEnvironment &env) {
    return new twExample(env);
}
```

---

### Step 5: Build & Test

```bash
# Build
./build.sh

# Verify compilation
# (should complete with 0 errors, 0 warnings from your code)

# Test basic functionality
./build/bin/smaragd.app/Contents/MacOS/smaragd
# - Create track
# - Add component instance
# - Play (verify no crashes)
# - Render (verify component used)
```

---

## Implementation Checklist

### Required Methods

- [ ] `getNInputs()` / `getNOutputs()` — channel counts
- [ ] `getInputName()` / `getOutputName()` — labels for UI
- [ ] `createOutputLatches()` — initialize output latches
- [ ] `init()` — one-time setup (called after wiring)
- [ ] `reset()` — clear state (called on timeline reset)
- [ ] `calcOutputTo(IOVector&, idx_t)` — type-safe rendering ⭐
- [ ] `calcOutputTo(sample_t*, length_t, idx_t)` — legacy compatibility

### Optional Methods

- [ ] `captureInternalState()` — for stateful components (return empty any if stateless)
- [ ] `restoreInternalState(const std::any&)` — for stateful components
- [ ] `getOutputCaps()` / `narrowCaps()` / `commitFormats()` — for format negotiation
- [ ] `setBufferSize(length_t)` — if buffer-size aware

### Recommended Practices

- [ ] Use `alloca()` for temp buffers in hot paths (stack-based, efficient)
- [ ] Use `std::lock_guard<std::mutex>` for thread safety
- [ ] Document `_nolock` helpers with "Caller must hold mutex()"
- [ ] Implement `captureInternalState()` for any state that needs persistence
- [ ] Use IOVector for all new code (type-safe, bounds-checked)
- [ ] Keep raw-pointer version simple (can delegate to IOVector)
- [ ] Test with `./build/bin/action_roundtrip_test` (exercises all components)

---

## Example: Complete Simple Component

Here's a minimal gain processor:

### Header: `tw303a/include/twgain.h`
```cpp
#ifndef _TW_GAIN_H_
#define _TW_GAIN_H_

#include "twcomponent.h"

class twGain : public twComponent {
public:
    twGain(tw303aEnvironment &env);
    virtual ~twGain();

    virtual idx_t getNInputs() const override { return 1; }
    virtual idx_t getNOutputs() const override { return 1; }
    virtual const char *getInputName(idx_t) const override { return "Input"; }
    virtual const char *getOutputName(idx_t) const override { return "Output"; }

    virtual void createOutputLatches() override;
    virtual void init() override;
    virtual void reset() override {}

    // New interface (type-safe)
    virtual length_t calcOutputTo(IOVector& dest, idx_t idx) override;

    // Legacy interface (wraps new)
    virtual length_t calcOutputTo(sample_t *pDest, length_t length, idx_t idx) override;

    void setGainDb(double gainDb) {
        std::lock_guard<std::mutex> lock(mutex());
        gainDb_ = gainDb;
    }

private:
    double gainDb_ = 0.0;
};

#endif
```

### Source: `tw303a/src/twgain.cc`
```cpp
#include "twgain.h"
#include "tw303aenv.h"
#include "io_vector.h"

twGain::twGain(tw303aEnvironment &env)
    : twComponent(env), gainDb_(0.0)
{
}

twGain::~twGain()
{
}

void twGain::createOutputLatches()
{
    pOutputLatches[0] = new twStreamingLatch(*this, 0, 0);
}

void twGain::init()
{
    twComponent::init();
}

// NEW: Type-safe IOVector version
length_t twGain::calcOutputTo(IOVector& dest, idx_t outChannel) override
{
    std::lock_guard<std::mutex> lock(mutex());

    sample_t *buffer = (sample_t *)alloca(dest.length() * sizeof(sample_t));
    readInput(0, buffer, dest.length(), outChannel);

    // Apply gain
    double factor = pow(10.0, gainDb_ / 20.0);
    for (size_t i = 0; i < dest.length(); i++) {
        buffer[i] *= (sample_t)factor;
    }

    return dest.copyFrom(IOVector::CreateFromBuffer(buffer, dest.length()), 0, dest.length());
}

// LEGACY: Raw-pointer version (wraps IOVector)
length_t twGain::calcOutputTo(sample_t *pDest, length_t length, idx_t idx) override
{
    std::lock_guard<std::mutex> lock(mutex());

    // Simple approach: call IOVector version via temp wrapper
    auto page = std::make_shared<twOutputPage>();
    IOVector vec(page, 0, length);
    length_t result = calcOutputTo(vec, idx);
    memcpy(pDest, page->samples.data(), result * sizeof(sample_t));
    return result;
}
```

---

## Common Mistakes to Avoid

### ❌ Mistake 1: Not Implementing Both Interfaces
```cpp
// WRONG: Only IOVector
virtual length_t calcOutputTo(IOVector& dest, idx_t idx) override {}

// RIGHT: Implement both
virtual length_t calcOutputTo(IOVector& dest, idx_t idx) override {}
virtual length_t calcOutputTo(sample_t *pDest, length_t length, idx_t idx) override {}
```

### ❌ Mistake 2: Forgetting Thread Safety
```cpp
// WRONG: Direct state access
void setGain(double gain) { gainDb_ = gain; }  // Race!

// RIGHT: Use mutex
void setGain(double gain) {
    std::lock_guard<std::mutex> lock(mutex());
    gainDb_ = gain;
}
```

### ❌ Mistake 3: Not Capturing Internal State
```cpp
// WRONG: Stateful component without snapshots
// (Will lose state at page boundaries)

// RIGHT: Implement state snapshots
virtual std::any captureInternalState() const override;
virtual void restoreInternalState(const std::any& state) override;
```

### ❌ Mistake 4: Allocating on Heap in Hot Path
```cpp
// WRONG: New allocation per render
sample_t *buffer = new sample_t[length];
// ... use buffer ...
delete[] buffer;

// RIGHT: Stack allocation (alloca)
sample_t *buffer = (sample_t *)alloca(length * sizeof(sample_t));
// ... use buffer ... (auto-freed on return)
```

### ❌ Mistake 5: Not Handling Bounds
```cpp
// WRONG: Raw pointer (caller might lie about length)
pDest[1000] = value;  // Buffer overflow if length < 1001

// RIGHT: IOVector validates bounds
dest.copyFrom(src, destOffset, numFrames);  // Throws if out of bounds
```

---

## Testing Your Component

### Unit Test (within action_roundtrip_test)

1. Create a simple project with your component
2. Place a clip on a track
3. Play (verify component doesn't crash)
4. Render to WAV (verify output correct)
5. Check that state is preserved across pages

### Manual Test

```bash
# Build
./build.sh

# Run tests
./build/bin/action_roundtrip_test

# Launch UI
./build/bin/smaragd.app/Contents/MacOS/smaragd

# In UI:
# 1. File → New
# 2. Add Track
# 3. Clip → Add Sample (or create container)
# 4. Play (should hear audio with your component)
# 5. File → Render (should complete without errors)
```

---

## Troubleshooting

### Build Error: "Unknown type 'IOVector'"
**Solution:** Add `#include "io_vector.h"` to your .cc file

### Build Error: "'calcOutputTo' does not override"
**Solution:** Add `override` keyword to method signature

### Runtime: "Segmentation fault in calcOutputTo"
**Solution:** 
- Check bounds in IOVector operations
- Verify `dest.length()` is correct
- Ensure temp buffers are large enough
- Check input reads don't exceed available data

### Runtime: "Audio is silent"
**Solution:**
- Verify `readInput()` returns data
- Check gain/scale factors
- Ensure `return` value is correct (number of frames written)

### Runtime: "State mismatch" message
**Solution:**
- Verify `captureInternalState()` and `restoreInternalState()` match
- Check `std::any_cast<>()` uses correct type
- Test with simple state first (e.g., single float)

---

## Pattern Recommendations by Component Type

| Type | Pattern | Example | Complexity |
|------|---------|---------|------------|
| Constant/Generator | Direct fill | twConstant | Low |
| Simple processor | Stack alloc | twGain | Low |
| Filter | Multi-input | twMoog | Medium |
| Stateful effect | State snapshot | twPipe | Medium |
| Reader | Position tracking | twSampleReader | Medium |
| Multi-input | Accumulation | twMixer | Medium |
| Plugin wrapper | Delegation | twPluginInsert | High |

---

## Reference: IOVector API

```cpp
class IOVector {
public:
    // Construct from page
    IOVector(std::shared_ptr<twOutputPage> page, offset_t offset, length_t len);
    
    // Fill operations (bounds-safe)
    length_t fillConstant(offset_t dstOffset, length_t numFrames, sample_t value);
    length_t fillSilence(offset_t dstOffset, length_t numFrames);
    
    // Copy operations (zero-copy page sharing)
    length_t copyFrom(const IOVector& src, offset_t dstOffset, length_t numFrames);
    length_t copyFrom(const IOVector& src, offset_t srcOffset, 
                      offset_t dstOffset, length_t numFrames);
    
    // Mixing operations
    length_t mixFrom(const IOVector& src, offset_t dstOffset, length_t numFrames);
    
    // Accessors
    length_t length() const;
    float* data();
    const float* data() const;
    std::shared_ptr<twOutputPage> page() const;
    
    // Factory
    static IOVector CreateFromBuffer(const sample_t* buffer, length_t len);
    static IOVector CreateForPageOutput(std::shared_ptr<twOutputPage> page);
};
```

---

## Next Steps After Implementation

1. **Build:** Verify no compilation errors
2. **Test:** Run test suite (98/100 passing)
3. **Manual test:** Try in UI with real audio
4. **Document:** Add entry to `COMPONENTS.md`
5. **Code review:** Have another developer review IOVector usage
6. **Merge:** Commit to main with detailed commit message

---

**Last Updated:** 2026-06-30  
**For Questions:** See `ARCHITECTURE.md` and `COMPONENTS.md`  
**Example Components:** twConstant (simplest), twMoog (DSP), twLoopReader (stateful)
