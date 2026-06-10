# 06. Recording MVP Implementation Plan

## Overview

Implement audio recording to armed tracks with real-time file writing and waveform expansion. Users arm tracks via ARM buttons, select input device, press record (Cmd-R / Ctrl-R / numpad *), and audio is captured to WAV files in project directory with timestamp + input index as filename.

## Design Summary

### MVP Scope
- **One recording session at a time** (blocks playback)
- **Per-input file writing** (one WAV per armed input, not per track)
- **ARM buttons per track** (multiple tracks can share one input)
- **Input device selection** (UI picker for available devices)
- **Live waveform updates** (every 0.1s during recording)
- **File format:** `YYYYMMDD_HHMMSS_mmm_input0.wav` (timestamp + milliseconds + input index)
- **Placement:** Recorded audio auto-placed as cuts on armed tracks
- **Start position:** Left locator (time range indicator)

### Not in MVP
- Per-track input channel selection (all tracks on same input for now)
- Multiple simultaneous recordings
- Stop/pause during recording
- Recording level metering
- Audio input routing per track

## Architecture: Key Components

### Audio Input Path
- **Platform abstraction:** `AudioInput` interface (pull-based, callback-free)
- **Implementations:** 
  - Windows: WASAPI loopback / input device
  - Linux: ALSA input
  - macOS: CoreAudio input
- **Device enumeration:** Similar to `AudioBackend`, list available input devices

### Recording Session
- **RecordingSession** class (background thread)
  - Pulls audio from input device
  - Writes to WAV file (one per input)
  - Emits progress callbacks (every 0.1s)
  - Tracks per-track ARM state
  - Syncs to project locator

### UI Components
- **ARM button per track** (in track header)
- **Record button** (in playback controls, next to Play/Stop)
- **Input device picker** (in Audio menu or dialog)
- **Recording indicator** (in status bar during recording)

### File Management
- **Filename:** `{ProjectDir}/{YYYYMMDD_HHMMSS_mmm}_input{N}.wav`
- **Location:** Saved alongside project `.smaragd` file
- **Cleanup:** If recording cancelled, delete incomplete files

### Track Integration
- **Create cuts** on armed tracks from recorded WAV
- **Auto-expand cuts** during recording (every 0.1s to match recorded duration)
- **Locator sync:** Recording starts at left locator position
- **Multiple tracks:** Same input can be armed on multiple tracks (all get same audio)

---

## Implementation Phases

### Phase 1: Audio Input Abstraction (Backend)
**Goal:** Define and implement platform-specific audio input capture.

**Tasks:**
- [ ] Create `tw303a/include/audio/audio_input.h`
  - `AudioInput` abstract base class
  - `AudioInputConfig` (channels, sample rate, format)
  - `AudioInputDevice` (id, name, channel count)
  - `createAudioInput()` factory function

- [ ] Implement platform-specific inputs:
  - [ ] `tw303a/src/audio/wasapi_input.cc` (Windows)
  - [ ] `tw303a/src/audio/alsa_input.cc` (Linux)
  - [ ] `tw303a/src/audio/coreaudio_input.cc` (macOS)

- [ ] Update CMakeLists.txt to include input sources

**Dependencies:** WASAPI, ALSA, CoreAudio (same as playback backends)

---

### Phase 2: Recording Session & File Writer
**Goal:** Background thread that pulls from input, writes WAV, manages recording state.

**Tasks:**
- [ ] Create `tw303a/include/recording_session.h`
  ```cpp
  struct RecordingParams {
    std::string inputDeviceId;
    std::vector<std::string> armedTrackIds;
    std::string projectDirectory;
    double startTimeSeconds;  // locator position
    std::uint32_t sampleRate;
  };

  class RecordingSession {
    bool start(const RecordingParams& params);
    void stop();
    void requestStop();
    bool isRunning() const;
    double recordedDurationSeconds() const;
    const char* errorMessage() const;
    
    std::function<void(double duration)> onProgress;
    std::function<void(bool success, const std::vector<std::string>& filenames)> onComplete;
  };
  ```

- [ ] Implement `tw303a/src/recording_session.cc`
  - Background thread pulls from input device
  - Writes to WAV file (one per input)
  - Progress callbacks every 0.1s
  - Handles stop/cancel gracefully
  - Returns list of created files

- [ ] Update CMakeLists.txt

---

### Phase 3: SApplication Recording Integration
**Goal:** Wire recording session into app, manage playback/record exclusivity.

**Tasks:**
- [ ] Add recording session member to `SApplication`
- [ ] Add `recordingSession()` getter
- [ ] Add `isRecordingActive()` state check
- [ ] Add `startRecording()` method
  - Block playback if active
  - Validate armed tracks
  - Create recording files
  - Spawn session

---

### Phase 4: Per-Track ARM Buttons
**Goal:** Add UI to arm/disarm tracks for recording.

**Tasks:**
- [ ] Extend track header UI to include ARM button
  - Checkbox or toggle button
  - Visual indicator when armed
  - Connected to `STrack::setArmedForRecording(bool)`

- [ ] Add `SProject` tracking of armed tracks
  - `void setTrackArmed(const QString& trackId, bool armed)`
  - `bool isTrackArmed(const QString& trackId) const`
  - `QList<QString> getArmedTrackIds() const`

---

### Phase 5: Record Button & Keyboard Shortcuts
**Goal:** Add record control to playback toolbar.

**Tasks:**
- [ ] Add record button next to Play/Stop in `SMainWindow`
  - Connected to `onRecordTriggered()` slot
  - Button enabled only if ≥1 track armed

- [ ] Add keyboard shortcuts:
  - Cmd-R / Ctrl-R → toggle record
  - Numpad * (asterisk) → toggle record

- [ ] Implement `SMainWindow::onRecordTriggered()`
  - Show input device picker if not selected
  - Validate armed tracks
  - Call `SApplication::startRecording()`
  - Show recording progress UI (status bar + live duration)

---

### Phase 6: Input Device Selection
**Goal:** UI to select audio input device.

**Tasks:**
- [ ] Create `SInputDeviceDialog` or add to Audio menu
  - List available input devices
  - Show channel count per device
  - Set default / remember selection in `SSettings`

- [ ] Add `SApplication` method to select input device
  - Validate device exists
  - Store in `SSettings::inputDeviceId()`

---

### Phase 7: Recording Progress & Status
**Goal:** Real-time UI feedback during recording.

**Tasks:**
- [ ] Add recording indicator to status bar
  - "Recording: 00:12.3s (input0)"
  - Updates every 0.1s
  - Shows which input is active

- [ ] Prevent user actions during recording
  - Disable play/stop (only stop recording)
  - Lock track ARM buttons
  - Show "Recording in progress" message

---

### Phase 8: Cut Placement & Track Integration
**Goal:** Place recorded audio as cuts on armed tracks; expand during recording.

**Tasks:**
- [ ] After recording completes:
  - For each armed track:
    - Create `SCut` object wrapping recorded WAV
    - Add cut to track at locator position
    - Set cut duration = recorded duration

- [ ] During recording (progress callback):
  - Update all cut durations to match recorded duration
  - Trigger waveform re-render

- [ ] Handle cleanup:
  - If recording cancelled, delete incomplete WAV files
  - If placement fails, offer undo

---

### Phase 9: Documentation & Testing
**Goal:** Document recording feature and testing strategy.

**Tasks:**
- [ ] Update CLAUDE.md with "Recording Audio" section
- [ ] Add to docs/BUILD.md: Recording testing checklist
- [ ] Manual testing:
  - [ ] Arm single / multiple tracks
  - [ ] Record with Cmd-R, Ctrl-R, numpad *
  - [ ] Verify WAV files created with correct naming
  - [ ] Check cuts placed on armed tracks
  - [ ] Verify live expansion during recording
  - [ ] Test cancel mid-record (file cleanup)
  - [ ] Test across platforms (Windows, macOS, Linux)

---

## Data Structures

### STrack Extensions
```cpp
class STrack {
public:
  void setArmedForRecording(bool armed);
  bool isArmedForRecording() const;
  
private:
  bool armedForRecording_ = false;
};
```

### SProject Extensions
```cpp
class SProject {
public:
  void setTrackArmed(const QString& trackId, bool armed);
  bool isTrackArmed(const QString& trackId) const;
  QList<QString> getArmedTrackIds() const;
  
private:
  QSet<QString> armedTracks_;
};
```

### SApplication Extensions
```cpp
class SApplication {
public:
  audio::RecordingSession* recordingSession() const;
  bool isRecordingActive() const;
  void startRecording();
  void selectInputDevice(const std::string& deviceId);
  
private:
  std::unique_ptr<audio::RecordingSession> recordingSession_;
  std::string selectedInputDeviceId_;
};
```

---

## Implementation Order

1. **Phase 1** — Audio input abstraction (independent of UI)
2. **Phase 2** — Recording session (core logic)
3. **Phase 3** — SApplication integration
4. **Phase 4** — ARM buttons (simple UI)
5. **Phase 5** — Record button + shortcuts
6. **Phase 6** — Device picker (optional initially)
7. **Phase 7** — Progress UI
8. **Phase 8** — Cut placement + track sync
9. **Phase 9** — Documentation & testing

---

## Unknowns / Decisions

1. **File naming:** Should milliseconds be included? (Current: yes, for precision)
2. **Multi-input:** Should we record multiple inputs simultaneously in MVP? (Current: No, assume 1 input per session)
3. **Level metering:** Skip for MVP, add later if needed?
4. **Pre-roll:** Should there be pre-roll before recording starts? (Current: No, start immediately at locator)
5. **Undo:** Should recording + placement be a single undo action? (Current: Yes, `SAction` wrapper)

---

## Success Criteria

- ✅ ARM buttons visible on tracks, state persists
- ✅ Record button triggers recording on Cmd-R / Ctrl-R / numpad *
- ✅ Input device picker shows available devices
- ✅ WAV files created in project directory with timestamp + input index
- ✅ Audio placed as cuts on armed tracks at locator position
- ✅ Cuts auto-expand every 0.1s during recording to match duration
- ✅ Recording blocks playback (one player at a time)
- ✅ Cancel/stop cleans up incomplete files gracefully
- ✅ Works on Windows (WASAPI), macOS (CoreAudio), Linux (ALSA)
- ✅ Status bar shows recording progress (duration, input device)
