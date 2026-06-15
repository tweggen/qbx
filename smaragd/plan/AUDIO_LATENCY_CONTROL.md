# Audio Latency & Buffer Size Control

## Goal

Enable user-configurable buffer sizes and derive platform-specific latencies to align playback and recording accurately.

## Problem Statement

Currently:
- Buffer sizes are hardcoded per platform
- No latency measurement or reporting to user
- Recording and playback don't account for latency differences; simultaneous recording and playback may be misaligned
- No user control over the latency/responsiveness tradeoff

## Solution Overview

Implement a three-phase approach:

### Phase 1: Read-Only Latency Reporting (This phase)
Query and expose measured latencies per platform. No user control yet; just visibility.

### Phase 2: Platform-Specific Buffer Control (Future)
Implement user-facing buffer size options per platform capabilities.

### Phase 3: Recording/Playback Sync (Future)
Apply latency compensation when recording simultaneous with playback.

---

## Phase 1: Read-Only Latency Reporting

### Changes to Audio Interfaces

#### 1.1 `tw303a/include/audio/audio_backend.h`

Add to `AudioConfig`:
```cpp
struct AudioConfig {
    // existing fields...
    uint32_t sampleRate, channels, bufferFrames, periodFrames;
    twSampleType sampleType;
    
    // NEW:
    uint32_t outputLatencyFrames = 0;  // total latency: device + driver + app
};
```

Add query method:
```cpp
class AudioBackend {
    // existing...
    virtual AudioConfig getConfig() const = 0;
    
    // NEW:
    virtual uint32_t getLatencyFrames() const { return getConfig().outputLatencyFrames; }
};
```

#### 1.2 `tw303a/include/audio/audio_input.h`

Add to `AudioInputConfig`:
```cpp
struct AudioInputConfig {
    // existing fields...
    uint32_t sampleRate, channels, bufferFrames;
    twSampleType sampleType;
    
    // NEW:
    uint32_t inputLatencyFrames = 0;  // total latency: device + driver + app
};
```

Add query method:
```cpp
class AudioInput {
    // existing...
    virtual const AudioInputConfig &getConfig() const = 0;
    
    // NEW:
    virtual uint32_t getLatencyFrames() const { return getConfig().inputLatencyFrames; }
};
```

### 1.3 Platform Implementations

#### Windows (WASAPI): `wasapi_backend.cc` / `wasapi_input.cc`

- Call `IAudioClient::GetStreamLatency()` after device is opened
- Store result in `config_.outputLatencyFrames` / `config_.inputLatencyFrames`
- Add device buffer and resampler latency to OS-reported value

**Files to modify:**
- `tw303a/src/audio/wasapi_backend.cc` → `openDevice()`: call `GetStreamLatency()`, update `config_`
- `tw303a/src/audio/wasapi_input.cc` → `openDevice()`: same

#### macOS (CoreAudio): `coreaudio_backend.cc` / `coreaudio_input.mm`

- Call `AudioUnitGetProperty(kAudioUnitProperty_Latency, ...)` on output/input unit
- Store result in `config_.outputLatencyFrames` / `config_.inputLatencyFrames`
- Add resampler latency to unit-reported value

**Files to modify:**
- `tw303a/src/audio/coreaudio_backend.cc` → `openDevice()`: query unit latency after `AudioUnitInitialize()`
- `tw303a/src/audio/coreaudio_input.mm` → `openDevice()`: query AVAudioEngine latency

#### Linux (ALSA): `alsa_backend.cc` / `alsa_input.cc`

- Call `snd_pcm_info_get_delay()` or estimate from buffer/period settings
- Store result in `config_.outputLatencyFrames` / `config_.inputLatencyFrames`

**Files to modify:**
- `tw303a/src/audio/alsa_backend.cc` → `openDevice()`: query/calculate latency
- `tw303a/src/audio/alsa_input.cc` → `openDevice()`: same

#### Null fallback: `null_backend.cc` / `null_input.cc`

- Set to reasonable defaults (e.g., 1024 frames ≈ 21ms at 48kHz)

### 1.4 UI Display

Add latency display to **Edit → Options → Audio** tab:

```
Buffer size:        1024 frames
Input device:       [dropdown]
Input latency:      ~45 ms (device + resampler)
Output latency:     ~48 ms (device + resampler)
Round-trip latency: ~93 ms
```

**Files to modify:**
- `main/include/soptions.h` / `main/src/soptions.cpp` → Add audio latency display
- Update settings dialog to show the values

### 1.5 Resampler Latency

The linear resampler in `tw303a/include/twresampler.h` has inherent latency:
- Typically ~half the filter window size
- For now, estimate as `bufferFrames / 2` and add to total

**Files to check:**
- `tw303a/include/twresampler.h` / `tw303a/src/twresampler.cc` → Document latency property

---

## Phase 2: Platform-Specific Buffer Control (Deferred)

Not implemented in Phase 1. Design outline:

### 2.1 Interface Extensions

```cpp
class AudioBackend {
    virtual std::vector<uint32_t> getAvailableBufferSizes() const = 0;
    virtual int setBufferSize(uint32_t frameCount) = 0;
};
```

### 2.2 Per-Platform Strategy

| Platform | Approach |
|----------|----------|
| **WASAPI** | (a) Offer presets in shared mode (256/512/1024/2048), or (b) implement exclusive mode for full control |
| **CoreAudio** | Report device buffer; no user control (OS-managed) |
| **ALSA** | Full control via `snd_pcm_hw_params_set_buffer_size_near()` |

---

## Phase 3: Recording/Playback Sync (Deferred)

Not implemented in Phase 1. Design outline:

### 3.1 Changes to `RecordingSession`

```cpp
class RecordingSession {
    int startRecording() {
        // Query latencies from both endpoints
        uint32_t inputLatency = audioInput_->getLatencyFrames();
        uint32_t outputLatency = audioBackend_->getLatencyFrames();
        
        // Calculate sync offset: how many frames to shift the recording
        // to align with playback when both are happening simultaneously
        int32_t syncOffsetFrames = (int32_t)outputLatency - (int32_t)inputLatency;
        
        // Apply when placing the recorded clip on the timeline
        recordingStartFrame_ += syncOffsetFrames;
    }
};
```

### 3.2 Optional: Latency Measurement Tool

Add **Edit → Options → Audio → Measure Latency** button:
- Play a click track
- Record input
- Measure delay between peak in output and peak in input
- Display result to user

---

## Implementation Order

1. Update `AudioConfig` and `AudioInputConfig` structs
2. Add virtual methods to base classes
3. Implement WASAPI latency query
4. Implement CoreAudio latency query
5. Implement ALSA latency calculation
6. Update UI to display latencies
7. Test on all three platforms
8. Document in AUDIO_IO_ARCHITECTURE.md

## Testing

- Verify latency values are sensible (typically 10-100ms for consumer audio interfaces)
- Cross-check against OS/device settings
- Test with different buffer sizes and devices on each platform
- Verify reporting doesn't crash or block UI

## Files to Touch

**Core audio:**
- `tw303a/include/audio/audio_backend.h`
- `tw303a/include/audio/audio_input.h`
- `tw303a/src/audio/wasapi_backend.cc`
- `tw303a/src/audio/wasapi_input.cc`
- `tw303a/src/audio/coreaudio_backend.cc`
- `tw303a/src/audio/coreaudio_input.mm`
- `tw303a/src/audio/alsa_backend.cc`
- `tw303a/src/audio/alsa_input.cc`
- `tw303a/src/audio/null_backend.cc`
- `tw303a/src/audio/null_input.cc`

**UI:**
- `main/include/soptions.h`
- `main/src/soptions.cpp`
- `main/include/soptionsdialog.h` (or equivalent)
- `main/src/soptionsdialog.cpp` (or equivalent)

**Documentation:**
- `smaragd/docs/AUDIO_IO_ARCHITECTURE.md`

---

## Success Criteria

- ✅ Latency values are queryable via `getLatencyFrames()` on all backends
- ✅ UI displays measured latencies in settings dialog
- ✅ Values are reasonable (match OS/device specs within ±10%)
- ✅ No blocking or crashes when querying latencies
- ✅ Null backend has sensible defaults
