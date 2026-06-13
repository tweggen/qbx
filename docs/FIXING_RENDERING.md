# Fixing Rendering: Audio Path Alignment

## Test WAV File: test_sawtooth.wav

**Purpose:** Diagnostic tool for validating render/playback timing and sample-level accuracy.

**Specification:**
- **Format:** 16-bit PCM, mono, 48 kHz
- **Duration:** 21.85 seconds (1,048,576 samples)
- **Content:** 4 identical sawtooth ramps
  - Each ramp: -32768 to +32767 (65,536 distinct values)
  - Quantization: Each value held for exactly 4 samples
  - Total per ramp: 262,144 samples

**Usage:**
1. Load the file into Smaragd as a clip on a track
2. Render to WAV at project rate (default 48 kHz)
3. Compare byte-for-byte with original using `cmp` or waveform editor
4. Any timing drift or sample skew will manifest as waveform misalignment

**Why this design:**
- Sawtooth provides linear monotonic data (easy to verify visually and numerically)
- Quantized steps (4 samples each) expose sample-level timing errors
- 4 repetitions catch both consistent errors (persistent offset) and drift (cumulative)

---

## Playback Audio Path

**Trigger:** User clicks play button or presses spacebar; `twSpeaker::startOutput()` called.

### Device Setup (twspeaker.cc lines 26–86)

1. **Rate negotiation:** Determine graph rate (project sample rate) and device-supported rates
2. **Device open:** Call `backend_->openDevice(outputDeviceId_, graphRate)`
   - Backend attempts to open device at graph rate (passthrough case)
   - Falls back to device native rate if unavailable
3. **Resampler config:** `resampler_.configure(graphRate, deviceRate)`
   - If `graphRate == deviceRate`, resampler is passthrough (no interpolation)
   - Otherwise, resampler performs linear interpolation
4. **Backend callback registration:** `backend_->setRenderCallback([this](...) { ... })`
   - Callback invoked by audio backend when device buffer needs filling

### Real-Time Playback Callback (twspeaker.cc lines 87–120)

Device calls this callback ~20–40 times per second (buffer-dependent):

```cpp
[this](float *out, std::size_t frames, std::uint32_t channels) -> std::size_t {
    offset_t formerPos = SApplication::app().getGlobalLocatorPos();
    length_t inConsumed = 0;
    
    // Pull mono samples from synth via resampler
    length_t framesOut = resampler_.process(
        static_cast<twLatchStreamingOutput *>(pInputPlugs[0]),
        out, static_cast<length_t>(frames), &inConsumed);
    
    // Expand mono to stereo/multichannel
    if (channels > 1) {
        for (length_t i = framesOut - 1; i >= 0; --i) {
            float s = out[i];
            for (std::uint32_t c = 0; c < channels; ++c) {
                out[i * channels + c] = s;
            }
        }
    }
    
    // Advance locator by INPUT frames consumed (not output frames)
    SApplication::app().setGlobalLocatorPos(formerPos + inConsumed);
    return static_cast<std::size_t>(framesOut);
}
```

**Key behaviors:**
- **Source:** Synth's output plug, obtained via `pInputPlugs[0]` (wired at graph-construction time)
- **Rate matching:** Resampler handles graph↔device rate mismatch via linear interpolation
- **Position tracking:** Advances by `inConsumed` (input frames), not output frames, to account for resampling
- **Channel expansion:** Mono synth duplicated to N device channels in-place (avoids extra copy)

### Audio Graph Pull Chain

When resampler calls `pInputPlugs[0]->readData()`:
1. Input plug pulls from its connected synth output
2. Synth component's `calcNextFrames()` or streaming callback is invoked
3. Synth generates audio frame-by-frame, respecting current `seekTo()` position
4. Resampler buffers input history and interpolates to output rate
5. Callback returns to device with filled buffer

---

## Rendering Audio Path

**Trigger:** User selects File → Render... and confirms; `RenderSession::start()` called with synth component.

### Initialization (render_session.cc lines 23–97)

1. **Parameter validation:** Check output path, time range, format
2. **Writer creation:** `createAudioFileWriter(format)` — selects WAV/OGG/MP3 encoder
3. **File open:** Writer opens output file with stereo, 16-bit, project sample rate
4. **Thread spawn:** Start `renderThreadMain()` in background thread

### Render Thread Main Loop (render_session.cc lines 120–230)

```cpp
// Initial seek (once per render, before loop starts)
synthOutput_->seekTo(startOffsetSamples_);
SApplication::app().setGlobalLocatorPos(startOffsetSamples_);

// Configure resampler (passthrough, since render rate = project rate)
resampler_.configure(sampleRate_, sampleRate_);
resampler_.reserveHint((length_t) RENDER_BUFFER_FRAMES);
resampler_.reset();

// Get synth output (same wiring as playback)
twLatchOutput *synthOutputPlug = synthOutput_->linkOutput(0);

// Main render loop (driven by this thread, not device)
while (!cancelRequested_ && samplesWrittenVal < totalSamples_) {
    // Pull mono frames via resampler (same interface as playback callback)
    length_t inConsumed = 0;
    length_t framesGenerated = resampler_.process(
        static_cast<twLatchStreamingOutput *>(synthOutputPlug),
        buffer.data(), (length_t) framesToRender, &inConsumed);
    
    // Fill silence if synth produced no frames
    if (framesGenerated <= 0) {
        std::fill(buffer.begin(), buffer.begin() + framesToRender, 0.0f);
        framesGenerated = framesToRender;
    }
    
    // Expand mono to stereo (identical to playback callback)
    for (length_t i = framesGenerated - 1; i >= 0; --i) {
        float s = buffer[i];
        buffer[i * 2] = s;      // Left
        buffer[i * 2 + 1] = s;  // Right
    }
    
    // Write to file
    writer_->write(buffer.data(), framesGenerated);
    
    // Update position (same pattern as playback callback)
    offset_t formerPos = SApplication::app().getGlobalLocatorPos();
    SApplication::app().setGlobalLocatorPos(formerPos + framesGenerated);
    
    samplesWrittenVal += framesGenerated;
}
```

**Key behaviors:**
- **Same synth wiring:** Obtains output via `linkOutput(0)` (identical to speaker's input connection)
- **Same resampler:** Uses `twResampler::process()` (same frame-pulling interface as playback)
- **Rate:** Resampler configured in passthrough mode (render rate = project rate)
- **Position tracking:** Advances by frames written (delta-based, like playback callback)
- **Self-driven loop:** Thread controls iteration, not device callback
- **Mono→stereo:** Identical expansion logic to playback callback

### Progress Feedback

- Every ~50 ms: `onProgress` callback invoked with `(samplesWritten, totalSamples)`
- Progress dialog updates in real time
- User can cancel via "Stop Rendering" button → sets `cancelRequested_` flag

---

## Interface Alignment: Render vs. Playback

As of the refactoring, **rendering now uses the exact same audio-sourcing interface as playback:**

| Aspect | Playback | Rendering |
|--------|----------|-----------|
| **Synth source** | `speaker->pInputPlugs[0]` | `synth->linkOutput(0)` |
| **Source type** | `twLatchStreamingOutput*` | `twLatchStreamingOutput*` (via cast) |
| **Frame pulling** | `resampler_.process()` | `resampler_.process()` |
| **Resampler config** | `configure(graphRate, deviceRate)` | `configure(projectRate, projectRate)` |
| **Position advance** | `formerPos + inConsumed` | `formerPos + framesGenerated` |
| **Mono→stereo** | In-place expand loop | Identical in-place expand loop |
| **Seeking** | Implicit in streaming | Explicit `seekTo()` at start, then streaming |

**Differences:**
- **Timing:** Playback driven by device callback (~20–40 Hz); render driven by thread loop (~2400 Hz at 2048-frame chunks)
- **Rate conversion:** Playback may resample; render always passthrough
- **Latency:** Playback has fixed device buffer latency; render has no latency concern

---

## Why Alignment Matters

**Before refactoring:** Render called `calcOutputTo()` directly, bypassing the resampler and its synth interface. If the synth's streaming interface (`calcNextFrames`, input plug readback) differed from `calcOutputTo()` behavior, timing and sample output could diverge.

**After refactoring:** Both paths call the *exact same code path* to pull frames from the synth, ensuring **bit-identical output** when rendered at the project's native rate (no device resampling).

**Validation:** The `test_sawtooth.wav` file can be used to verify this:
1. Load into a track
2. Render to WAV
3. Compare bytes: should be identical if timing is correct
4. Misalignment or skipped samples indicate remaining timing bugs

---

## Synth Component Structure

The synth component hierarchy (simplified):

```
tw303aEnvironment (root synth engine)
├── twNegotiator (rate negotiation)
├── twRewire / twMixer (graph mixing)
├── twOscillators, Filters, etc. (signal processors)
└── linkOutput(0) → twLatchOutput* (streaming output plug)

Speaker (twSpeaker)
├── pInputPlugs[0] ← synth->linkOutput(0) (wired at app startup)
├── resampler_ (rate conversion)
├── backend_ (WASAPI/ALSA/CoreAudio)
└── startOutput() → device callback loop
```

The `linkOutput()` method wires the synth to the speaker's input plugs at graph-construction time. Rendering bypasses the speaker but uses the same `linkOutput()` to access the synth's output in the resampler.

---

## Future Work

1. **Verify timing:** Use `test_sawtooth.wav` to confirm render output matches expected sample counts
2. **Stress test:** Load complex graphs and long audio, render vs. play in parallel
3. **Edge cases:** Render from non-zero time (via startOffsetSamples_), non-trivial resampling
4. **Device resampling:** Test playback on a device with different native rate to validate resampler correctness
