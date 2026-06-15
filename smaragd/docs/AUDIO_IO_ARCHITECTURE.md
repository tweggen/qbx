# Audio I/O Architecture — Reader & Writer

This document describes the two halves of Smaragd's device audio I/O:

- the **writer** — `AudioBackend`, which pulls synthesized audio and renders it
  to an output device, and
- the **reader** — `AudioInput`, which captures audio from an input device.

Both are platform abstractions with one concrete implementation per backend.
The Windows path (WASAPI) is the reference implementation and the only one
regularly tested; ALSA (Linux), CoreAudio (macOS) and the Null fallback share
the same interfaces.

> Scope: this covers the *device* boundary. File readers/writers used for
> render/import (`AudioFileWriter` → WAV/OGG/MP3) are a separate concern; the
> recording path described below happens to hand its captured frames to a
> `WAVWriter`, noted where relevant.

---

## 1. The two interfaces

### Writer — `AudioBackend` (`include/audio/audio_backend.h`)

A **callback-pull** sink. The backend owns a realtime thread; whenever the
device needs more audio it invokes a `RenderCallback` the client installed:

```cpp
using RenderCallback =
    std::function<std::size_t(float *out, std::size_t frames, std::uint32_t channels)>;
```

Lifecycle and queries:

| Method | Purpose |
|--------|---------|
| `openDevice(name, preferredRate)` | Acquire a device; report the rate/format actually in force via `getConfig()`. `preferredRate == 0` means "no preference". |
| `closeDevice()` | Release the device. |
| `startOutput()` / `stopOutput()` | Start/stop the realtime render thread. |
| `isRunning()` | Whether the render thread is active. |
| `setRenderCallback(cb)` | Install the pull callback (before `startOutput`). |
| `getConfig()` | Rate, channels, buffer size, native sample type. |
| `supportedRates()` | Rates openable without host resampling (shared-mode → the single mix rate). |
| `enumerateDevices()` | Selectable output devices for the picker. |

### Reader — `AudioInput` (`include/audio/audio_input.h`)

A **poll/read** source. There is no internal thread; the client calls `read()`
in a loop on its own worker thread.

| Method | Purpose |
|--------|---------|
| `openDevice(id, preferredRate)` | Acquire an input device at (ideally) the requested rate. |
| `closeDevice()` | Release the device. |
| `startCapture()` / `stopCapture()` | Start/stop the capture stream. |
| `read(interleaved, frameCount)` | Pull up to `frameCount` interleaved float frames; returns frames read (`0` when none available, `-1` on error). |
| `getConfig()` | Rate, channels, buffer size, sample type actually in force. |
| `listDevices()` | Enumerate input devices. |
| `errorMessage()` | Text of the last failed operation. |

### Factories & platform selection

`createAudioBackend()` (`src/audio/audio_backend.cc`) and `createAudioInput()`
(`src/audio/audio_input.cc`) pick the implementation at compile time:

| Define | Writer | Reader |
|--------|--------|--------|
| `QBX_WIN_WASAPI` | `WASAPIBackend` | `WASAPIInput` |
| `QBX_LINUX_ALSA` | `ALSABackend` | `ALSAInput` |
| `QBX_MAC_COREAUDIO` | `CoreAudioBackend` | `CoreAudioInput` |
| *(none)* | `NullBackend` | `NullInput` |

---

## 2. Where they sit in the data flow

### Playback (writer)

```
synth graph (project rate)
      │  pull
      ▼
  twSpeaker  ── resampler (project rate → device rate) + format conversion
      │  RenderCallback
      ▼
  AudioBackend (WASAPIBackend)
      │
      ▼
   output device
```

`twSpeaker` (`src/twspeaker.cc`) owns the backend. On `startOutput()` it opens
the device at the graph rate, negotiates wire rates, configures a resampler to
bridge any residual project-rate↔device-rate mismatch, then installs a
`RenderCallback` that pulls from the synth graph (handling loop/cycle playback
and advancing the global playback locator). The callback runs **on the
backend's realtime audio thread**.

### Recording (reader)

```
   input device
      │
      ▼
  AudioInput (WASAPIInput)  ── read() loop
      │
      ▼
  RecordingSession (worker thread)
      │  optional resample (device rate → project rate)
      ▼
  AudioFileWriter (WAVWriter)  →  YYYYMMDD_HHMMSS_mmm_input0.wav
```

`RecordingSession` (`src/recording_session.cc`) spawns one worker thread that
drives the **entire** input lifecycle — `openDevice → startCapture → read
(loop) → stopCapture → closeDevice` — and writes captured frames to a WAV file.
A small linear resampler converts device-rate capture to the project rate when
the device couldn't deliver the project rate directly (see §4).

---

## 3. Lifecycle as explicit state machines

Both WASAPI classes model their lifecycle as an **explicit state enum guarded by
an object mutex**, rather than inferring state from which handles happen to be
non-null. Every public lifecycle method is a guarded transition, so the legal
ordering is readable directly from the code, and a half-completed open collapses
cleanly back to the closed state instead of leaving a partially-initialized
device behind.

### Writer — `WasapiState` (`wasapi_backend.h`)

```
Closed ──openDevice──► Opening ──► Open          (failure ──► Closed)
Open   ──startOutput─► Starting ──► Running       (failure ──► Open)
Running ──stopOutput─► Stopping ──► Open
{Open, Running} ──closeDevice──► Closing ──► Closed
```

- `state_` is guarded by `stateMutex_`. The transient states
  (`Opening/Starting/Stopping/Closing`) exist only for the duration of a call
  while the mutex is held; a concurrent observer never sees them.
- `openDevice` routes **every** failure through one `fail()` helper →
  `releaseDevice_()` → `Closed`. This eliminates the old "half-open" state
  (client activated but render service not yet acquired) that earlier code had to
  detect and defend against.
- `closeDevice` stops a running stream itself (it does not rely on the caller's
  ordering); the destructor does the same.

### Reader — `WasapiInputState` (`wasapi_input.h`)

```
Closed ──openDevice──► Open          (failure ──► Closed)
Open   ──startCapture─► Capturing     (failure stays Open)
Capturing ──stopCapture──► Open
{Open, Capturing} ──closeDevice──► Closed
```

- `state_` is guarded by `mutex_`. All public methods lock and transition
  explicitly.
- Failure paths in `openDevice` and the destructor go through
  `closeDeviceLocked_()`, which stops capture, releases all handles, balances the
  COM init exactly once, and returns to `Closed`.

### Composing transitions without re-locking

Both classes split each public method into a **locking wrapper** plus a
`*_locked_` helper that assumes the caller already holds the mutex. This lets one
transition compose another safely:

- writer: `closeDevice` / destructor → `stopOutputLocked_()`; `startOutput` /
  `stopOutput` are thin wrappers over `startOutputLocked_` / `stopOutputLocked_`.
- reader: `closeDeviceLocked_()` → `stopCaptureLocked_()`; the failure paths and
  destructor call the locked helpers directly.

---

## 4. Threading & concurrency model

### Writer — two threads

1. **Control thread** (the caller — `twSpeaker`, on the UI thread): drives
   `openDevice / startOutput / stopOutput / closeDevice` and the queries.
2. **Audio thread** (`thread_`, owned by `WASAPIBackend`): runs
   `audioThreadProc_` → `renderOnce_`, which invokes the `RenderCallback`.

Rules that make this sound:

- `stateMutex_` serialises **every** control-thread transition and guards
  `state_` together with all device handles, `config_`, `sampleFormat_`,
  `callback_` and the scratch buffer.
- The **audio thread never takes `stateMutex_`**. Doing so would add unbounded
  latency to the realtime path *and* deadlock the `thread_.join()` that
  `stopOutput` performs while holding the lock. Instead, the audio thread only
  runs while `state_ == Running`, and everything it reads is established before
  the thread is created and not released or mutated until after it is joined.
- The handshake to stop the audio thread is the `stopFlag_` atomic plus a
  `SetEvent` on the buffer-ready event; `stopOutput` sets the flag, signals the
  event, then joins.
- `isRunning` / `getConfig` / `setRenderCallback` / `supportedRates` read state
  under the lock. `setRenderCallback` must only be called while not running
  (the audio thread reads `callback_` lock-free).

### Reader — single worker thread

`WASAPIInput` has **no thread of its own**. The whole lifecycle is driven by the
single `RecordingSession` worker thread, and COM is initialized/uninitialized on
that same thread (COM apartments are per-thread), so the calls are naturally
serialised. `mutex_` nonetheless guards `state_` and every handle so the object
stays safe if a control call ever arrives concurrently (e.g. `listDevices` from
the UI thread, or a stop racing the read loop). `RecordingSession::requestStop()`
only flips an atomic *in the session*; it does not call into `AudioInput`, so the
read loop and teardown never overlap on the input object.

---

## 5. Sample rate & format handling

The engine is rate-aware; a resampler at the device boundary reconciles the
project rate with the device rate.

- **Writer:** WASAPI shared mode is locked to the OS mix rate, so a differing
  `preferredRate` cannot be honoured natively — `twSpeaker`'s resampler bridges
  it (a passthrough when they already match). `getConfig()` reports the device's
  native rate and binary sample format (float32 / int16 / int32); `renderOnce_`
  converts the interleaved float scratch buffer to that format via
  `twConvertFrames`.
- **Reader:** `WASAPIInput::openDevice` first asks the shared engine to deliver
  frames at the requested project rate using `AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM`
  (+ `SRC_DEFAULT_QUALITY`). If the driver refuses, it falls back to capturing at
  the native mix rate and reports that rate, leaving `RecordingSession`'s linear
  resampler to convert device-rate → project-rate before writing the WAV. Either
  way the recorded file matches the project rate.

---

## 6. Platform status

| Platform | Writer | Reader | Status |
|----------|--------|--------|--------|
| Windows  | `WASAPIBackend` | `WASAPIInput` | Reference; shared-mode only |
| Linux    | `ALSABackend` | `ALSAInput` | Implemented, untested since refactor |
| macOS    | `CoreAudioBackend` | `CoreAudioInput` | Output audible; input via AudioQueue |
| —        | `NullBackend` | `NullInput` | Silent fallback |

---

## 7. Key files

| File | Role |
|------|------|
| `include/audio/audio_backend.h` | Writer interface, `AudioConfig`, `RenderCallback`, device info |
| `include/audio/audio_input.h` | Reader interface, `AudioInputConfig`, device info |
| `src/audio/audio_backend.cc` | `createAudioBackend()` factory |
| `src/audio/audio_input.cc` | `createAudioInput()` factory |
| `include/audio/wasapi_backend.h` / `src/audio/wasapi_backend.cc` | Windows writer + `WasapiState` machine |
| `src/audio/wasapi_input.h` / `src/audio/wasapi_input.cc` | Windows reader + `WasapiInputState` machine |
| `src/twspeaker.cc` | Connects the synth graph to the writer (playback) |
| `src/recording_session.cc` | Drives the reader on a worker thread (recording) |
