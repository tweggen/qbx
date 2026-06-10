# 05. Render-to-File Feature Implementation Plan

## Overview

Implement a "Render..." dialog allowing export to WAV, OGG Vorbis, and optional MP3 formats. Users choose extent (Entire project or Time Selection), format, and output path. Rendering is modal and non-interactive, maintaining the "one player at a time" directive.

## Design Summary

### Supported Formats
| Format | Library | Dependency | Notes |
|--------|---------|-----------|-------|
| WAV | libsndfile | Required | PCM-only, no compression |
| OGG Vorbis | libvorbis/libvorbisenc | Required | Patent-free, high quality |
| MP3 | libmp3lame | Optional | User must copy binary to app dir |

### Render Extent
- **Entire project**: Start=0, end=`SProject::getDurationSeconds()`
- **Time Selection**: `SProject::getTimeSelection()` (only enabled if selection exists)

### UI Flow
1. **SRenderDialog**: Format selector, quality settings, extent radio buttons, file picker
2. **SRenderProgressDialog**: Modal progress bar, time remaining, cancel button
3. Main window disabled during render; re-enabled on completion or cancel

---

## Implementation Phases

### Phase 1: Dependency Setup
**Goal:** Ensure libsndfile and libvorbis are available cross-platform.

**Tasks:**
- [ ] Add `find_package(SndFile)` and `find_package(Vorbis)` to CMakeLists.txt
- [ ] Add helpful error message if libraries not found (directs to install via brew/apt/vcpkg)
- [ ] Update docs/BUILD.md with per-platform installation instructions
- [ ] Test CMake configuration on:
  - [ ] macOS (arm64) with Homebrew
  - [ ] Linux x64/arm64 with apt
  - [ ] Windows x64 with vcpkg

**Files:**
- `CMakeLists.txt` (root)
- `docs/BUILD.md`

---

### Phase 2: Core Audio Pipeline for File Output

**Goal:** Create abstraction to write audio to files, decoupled from device playback.

**Tasks:**
- [ ] Create `include/tw303a/audio/audio_file_writer.h`
  ```cpp
  class AudioFileWriter {
  public:
    virtual ~AudioFileWriter() = default;
    virtual bool open(const std::string& path, const AudioConfig& config) = 0;
    virtual bool write(const float* interleaved, size_t sampleCount) = 0;
    virtual bool close() = 0;
    virtual const char* errorMessage() const = 0;
  };
  ```

- [ ] Create `src/audio/wav_writer.cc` → AudioFileWriter subclass using libsndfile
- [ ] Create `src/audio/ogg_writer.cc` → AudioFileWriter subclass using libvorbis
- [ ] Create `src/audio/mp3_writer.cc` → AudioFileWriter with dynamic libmp3lame loading
  - Add runtime library search (app dir, system paths)
  - Add `isAvailable()` method to check if libmp3lame found
  - Graceful failure with user message if not found

- [ ] Update `src/audio/CMakeLists.txt` to link libsndfile and libvorbis

**Files:**
- `tw303a/include/audio/audio_file_writer.h`
- `tw303a/src/audio/wav_writer.h/cc`
- `tw303a/src/audio/ogg_writer.h/cc`
- `tw303a/src/audio/mp3_writer.h/cc`
- `tw303a/src/audio/CMakeLists.txt`

---

### Phase 3: Render Session & Engine Integration

**Goal:** Create a background render task that pulls audio from synth and writes to file.

**Tasks:**
- [ ] Create `tw303a/include/render_session.h`
  ```cpp
  struct RenderParams {
    enum Extent { EntireProject, TimeSelection };
    Extent extent;
    double startTimeSec, endTimeSec;
    AudioFormat format;  // WAV, OGG, MP3
    int quality;  // 0-10 for OGG, ignored for WAV/MP3
    std::string outputPath;
  };
  
  class RenderSession {
    bool start(const RenderParams& params);
    void cancel();
    bool isRunning() const;
    size_t samplesWritten() const;
    size_t totalSamples() const;
    const char* errorMessage() const;
    
    // Signal-like callbacks (Qt signals in UI layer)
    std::function<void(size_t, size_t)> onProgress;
    std::function<void(bool, const char*)> onComplete;
  };
  ```

- [ ] Implement `tw303a/src/render_session.cc`
  - Allocate AudioFileWriter based on format
  - Calculate total samples from extent + sample rate
  - Spawn background thread that pulls synth audio + writes
  - Emit progress callbacks every ~50ms
  - Handle cancellation gracefully (close file, return early)
  - Catch write errors (disk full, permission denied)

- [ ] Integrate with `SApplication`:
  - Add `SRenderSession* renderSession()` getter
  - Ensure only one of (devicePlayer, renderSession) is active at a time
  - Block device playback while render is active

**Files:**
- `tw303a/include/render_session.h`
- `tw303a/src/render_session.cc`
- `main/include/sapplication.h` (add renderSession getter)
- `main/src/sapplication.cc` (integration)

---

### Phase 4: Qt UI — Render Dialog

**Goal:** User-facing dialog to select format, extent, and output path.

**Tasks:**
- [ ] Create `main/include/srenderdialog.h`
  ```cpp
  class SRenderDialog : public QDialog {
    // Layout: format group, extent group, output path, quality slider
    // Signals: on format change → update quality options visible
  };
  ```

- [ ] Implement `main/src/srenderdialog.cc`
  - **Format group**: Radio buttons for WAV, OGG Vorbis, MP3
    - MP3 radio disabled + tooltip if libmp3lame not found
  - **Quality group**: 
    - WAV: bit depth dropdown (16/24/32-bit float)
    - OGG: quality slider 0-10 (default 6)
    - MP3: bitrate dropdown 128-320 kbps
  - **Extent group**: Radio buttons
    - "Entire project" (always enabled)
    - "Time Selection" (enabled iff `SProject::hasTimeSelection()`)
  - **Output path**: Text field + "Browse..." button (QFileDialog)
  - **Buttons**: Render, Cancel

- [ ] Add validation:
  - Output path not empty, writable directory
  - Warn if file exists (overwrite? yes/no)
  - If MP3 selected but unavailable, show warning + instructions

- [ ] Connect format radio button changes to update quality UI

**Files:**
- `main/include/srenderdialog.h`
- `main/src/srenderdialog.cc`

---

### Phase 5: Qt UI — Render Progress Dialog

**Goal:** Modal, non-interactive progress display during render.

**Tasks:**
- [ ] Create `main/include/srenderprogress.h`
  ```cpp
  class SRenderProgressDialog : public QDialog {
    void setTotalSamples(size_t total);
    void onProgress(size_t written, size_t total);  // Called from render thread
    void onComplete(bool success, const QString& error);
  };
  ```

- [ ] Implement `main/src/srenderprogress.cc`
  - **Layout:**
    - "Rendering to: /path/file.ogg" label
    - Progress bar (samples written / total)
    - Percentage + time display (2:34 / 4:00)
    - Estimated time remaining
    - Cancel button
  - **Styling:** Center dialog, disable main window interaction
  - **Signals:** Connect to SRenderSession progress/completion
  - **Cancel handling:** Call session->cancel(), wait for completion, close dialog

**Files:**
- `main/include/srenderprogress.h`
- `main/src/srenderprogress.cc`

---

### Phase 6: Integration with SMainWindow & SApplication

**Goal:** Wire menu action and enable/disable during render.

**Tasks:**
- [ ] Add menu item to `SMainWindow`:
  - File → Render... (or Tools → Render...)
  - Connected to slot `onRenderMenuTriggered()`

- [ ] Implement `SMainWindow::onRenderMenuTriggered()`:
  - Show SRenderDialog (modeless, user can cancel)
  - On dialog accept:
    - Disable main window controls (synth params, device picker, playback buttons)
    - Create SRenderProgressDialog (modal)
    - Call `SApplication::startRender(params)`
    - Connect progress/completion signals
    - On completion: re-enable controls, close progress dialog

- [ ] Add to `SApplication`:
  - `void startRender(const RenderParams&)` — validates, starts session
  - `bool isRenderingActive()` const — prevent simultaneous playback
  - Block device playback if render active; block render if playback active

- [ ] Update `SProject` (if needed):
  - Ensure `hasTimeSelection()`, `getTimeSelection()`, `getDurationSeconds()` exist
  - Verify time selection persists across render (shouldn't affect it)

**Files:**
- `main/include/smainwindow.h` (add slots)
- `main/src/smainwindow.cc`
- `main/include/sapplication.h` (add render methods)
- `main/src/sapplication.cc`

---

### Phase 7: Documentation & Testing

**Tasks:**
- [ ] Update `docs/BUILD.md`:
  - Add "Dependencies" section with per-platform install instructions
  - macOS: `brew install libsndfile libvorbis`
  - Linux: `sudo apt install libsndfile1-dev libvorbis-dev`
  - Windows: vcpkg instructions + CMAKE_TOOLCHAIN_FILE note

- [ ] Add section to CLAUDE.md: "Rendering Audio"
  - Explain supported formats, extent options, modal behavior
  - Note: MP3 requires user-provided binary

- [ ] Manual testing checklist:
  - [ ] Open SRenderDialog, verify format/extent/output options
  - [ ] Render entire project to WAV, verify file plays
  - [ ] Render entire project to OGG, verify file plays
  - [ ] Render time selection (if selection exists) to WAV
  - [ ] Render with cancel button mid-way, verify cleanup
  - [ ] Try MP3 without binary, verify helpful error message
  - [ ] Verify main window is non-interactive during render
  - [ ] Verify synth cannot be played while rendering
  - [ ] Test on Windows, macOS, Linux

- [ ] Check for regressions:
  - Device playback still works
  - Device selection/switching still works
  - Project open/save/new unaffected

**Files:**
- `docs/BUILD.md` (update)
- `CLAUDE.md` (update)

---

## Implementation Order

1. **Phase 1** (Deps) — quick sanity check that libs exist
2. **Phase 2** (Writers) — independent of UI; test individually
3. **Phase 3** (Session) — core logic; testable with simple harness
4. **Phase 4** (Dialog) — UI scaffolding
5. **Phase 5** (Progress) — UI for feedback
6. **Phase 6** (Integration) — wire everything together
7. **Phase 7** (Docs & Test) — document, manual QA

---

## Unknowns / Decisions

1. **Time Selection UI**: Does the project already have in/out markers in a timeline view? Clarify what `SProject::getTimeSelection()` returns (sample indices or seconds?).
2. **Progress granularity**: How often should progress callback fire? (Proposal: every ~50ms or per audio chunk, whichever is longer, to avoid Qt signal spam.)
3. **Sample rate during render**: Should render always use the project's sample rate, or offer user resampling? (Proposal: always project rate; resampling is future scope.)
4. **Buffer size during render**: What chunk size for pulling from synth? (Proposal: 2048 samples, same as device backend.)

---

## Success Criteria

- ✅ User can select WAV or OGG format, render entire project or time selection
- ✅ Progress dialog shows smooth feedback, cancellable
- ✅ Output file is valid, playable in external player
- ✅ MP3 option present but gracefully fails with helpful message if library missing
- ✅ Main window is non-interactive during render
- ✅ Device playback blocked while rendering (and vice versa)
- ✅ No crashes, resource leaks, or file corruption on cancel
- ✅ Works on Windows (MinGW), macOS (arm64), Linux (x64/arm64)
