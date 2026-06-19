# 12 — Test Output Artifacts (Screenshots & Renders)

**Status:** 🔵 Proposed  
**Depends on:** `11_ACTION_SCRIPT_TEST_CASES.md` (completed)  
**Related to:** Headless testing, visual debugging, CI/CD artifact collection

## 1. Goal

Extend the action script protocol to emit **visual and audio artifacts** during test execution, collected to a configurable output directory. This enables:

1. **Claude-aided visual debugging** — Claude Code instances can inspect screenshots to diagnose layout, positioning, or rendering issues directly from test outputs.
2. **CI/CD artifact collection** — Test runs produce structured outputs (images, audio renders) for inspection, regression tracking, and historical comparison.
3. **Unified artifact model** — Screenshots, audio renders, and recorded clips all follow the same output-directory + filename pattern.

Example workflow:
```bash
smaragd.exe --test-case tests/cases/layout_debug.qxa \
            --test-output-dir ./test-results/run-2026-06-19 \
            -platform offscreen

# Output:
# test-results/run-2026-06-19/
#   ├── 01_single_track.png
#   ├── 01_single_track_half.png
#   ├── 02_grouped.png
#   ├── 02_grouped_custom.png
#   └── test_report.json  # or TAP summary
```

## 2. New components

### 2.1 `SScreenshotAction` — `main/include/actions/`

A new action class that captures the main window and writes to the output directory.

```cpp
// main/include/actions/sscreenshotaction.h
class SScreenshotAction : public SAction {
public:
    enum class Resolution { Full, Half, Custom };
    
    QString filename() const { return filename_; }
    Resolution resolution() const { return resolution_; }
    int customWidth() const { return customWidth_; }
    int customHeight() const { return customHeight_; }
    
    SApplyResult apply(SProject *proj, SApplication *app) override;
    void writeXml(QDomElement &el) const override;
    void readXml(const QDomElement &el) override;
    QString name() const override { return "screenshot"; }
    int formatVersion() const override { return 1; }
    
private:
    QString filename_;           // e.g., "01_single_track.png"
    Resolution resolution_;      // 100%, 50%, or explicit WxH
    int customWidth_ = 0;        // Only if resolution == Custom
    int customHeight_ = 0;
};
```

**Serialization:**
```xml
<!-- Full resolution -->
<screenshot filename="01_track.png" resolution="100%"/>

<!-- Half resolution (50% width/height = 25% of pixels) -->
<screenshot filename="01_track_half.png" resolution="50%"/>

<!-- Explicit dimensions -->
<screenshot filename="01_track_custom.png" resolution="800x600"/>
```

**Execution logic:**
1. Retrieve output directory from `SApplication::testOutputDir()` (or env var fallback).
2. Grab main window via `QApplication::primaryScreen()->grabWindow(mainWindowId)`.
3. Scale to target resolution.
4. Write to `<output-dir>/<filename>` as PNG.
5. Return `SApplyResult{applied: true, rejected: false}` on success; `{applied: false, rejected: true, reason: "..."}` on failure (e.g., write permission, invalid path).

### 2.2 Output directory context — `SApplication` + `main.cpp`

Add output-directory state to `SApplication` and wire command-line parsing:

```cpp
// main/include/sapplication.h (new methods)
class SApplication : public QApplication {
    // ...
public:
    void setTestOutputDir(const QString &path);
    QString testOutputDir() const;  // Returns path, or empty if not set
    bool ensureOutputDirExists() const;  // Create if needed; return success
    
private:
    QString testOutputDir_;
};
```

**Command-line parsing** in `main/src/main.cpp`:

```cpp
QCommandLineParser parser;
parser.addOption(QCommandLineOption(
    {"test-output-dir", "o"},
    "Directory for test artifacts (screenshots, renders).",
    "path"
));
parser.addOption({"run-actions", "Execute an action script.", "file"});
parser.addOption({"test-case", "Run an action script as a test.", "file"});
parser.addOption({"list-actions", "List known action verbs and exit."});

parser.process(app);

// Resolve output dir: CLI flag → env var → not set
QString outputDir = parser.value("test-output-dir");
if (outputDir.isEmpty()) {
    outputDir = QString::fromStdString(
        std::getenv("SMARAGD_TEST_OUTPUT_DIR") ?: ""
    );
}
if (!outputDir.isEmpty()) {
    app.setTestOutputDir(outputDir);
    if (!app.ensureOutputDirExists()) {
        qWarning() << "Failed to create output directory:" << outputDir;
        return 1;
    }
}
```

### 2.3 `SRenderAction` (future, non-blocking)

Placeholder for rendering audio during a test. Same pattern:

```xml
<render filename="output.wav" format="wav" fromTime="0" toTime="auto"/>
```

This reuses the existing render infrastructure but scripted. Deferred to Phase 2 or later.

## 3. Integration with `SActionRunner`

No changes needed to the runner itself; `SScreenshotAction` is a regular action that reads the output directory from `SApplication` when executed. The runner drains actions as normal.

**Output directory availability:**
- Actions access via `SApplication::instance().testOutputDir()`.
- If unset, the action fails gracefully (e.g., `applied: false, reason: "No output directory configured"`).

## 4. Headless (offscreen) support

Qt's offscreen platform supports window grabbing via `QScreen::grabWindow()`, so no special handling is needed. Test scripts work identically with or without a display:

```bash
# With display (development)
smaragd.exe --test-case test.qxa --test-output-dir ./out

# Headless (CI/CD)
smaragd.exe --test-case test.qxa --test-output-dir ./out -platform offscreen
```

## 5. Example test case

```xml
<?xml version="1.0" encoding="UTF-8"?>
<SActionScript version="1" name="layout_debug">

  <setup project="new"/>

  <actions>
    <!-- Create a single track and capture at full resolution -->
    <add-track index="-1"/>
    <screenshot filename="01_single_track.png" resolution="100%"/>
    <screenshot filename="01_single_track_half.png" resolution="50%"/>

    <!-- Add a second track and group them -->
    <add-track index="-1"/>
    <reparent-track sources="1" dest="0"/>
    <screenshot filename="02_grouped.png" resolution="100%"/>
    <screenshot filename="02_grouped_custom.png" resolution="800x600"/>

    <!-- Adjust volume and capture -->
    <set-track-volume trackIndex="0" newVolume="-6.0"/>
    <screenshot filename="03_volume_adjusted.png" resolution="100%"/>
  </actions>

  <assertions>
    <assert-track-count equals="1"/>
    <assert-track-volume trackIndex="0" equals="-6.0" epsilon="0.001"/>
  </assertions>

  <verify-undo restoresInitialState="true"/>

</SActionScript>
```

## 6. Implementation phases

### Phase 1 — Screenshot action (foundation, ~½ day)
- Implement `SScreenshotAction` with XML serialization.
- Add output-directory plumbing to `SApplication` and `main.cpp`.
- Register `SScreenshotAction` in `SActionRegistry`.
- Unit test: parse a `.qxa` with screenshots, execute, verify files written.
- Test on all three platforms (Windows/Linux/macOS) + offscreen.

### Phase 2 — Extend test report output (optional, ~¼ day)
- Modify `SActionRunner::Result` or TAP output to reference artifact paths.
- Example: test report JSON lists `artifacts: ["01_single_track.png", ...]`.
- Enables CI systems to collect and archive screenshots per run.

### Phase 3 — Render action (future, non-blocking)
- Implement `SRenderAction` to export audio during test execution.
- Same output-directory pattern.
- Useful for functional correctness of synth output (e.g., "this action should produce silence").

## 7. Risks & open questions

1. **Platform window grabbing.** Verify `QScreen::grabWindow()` works on all three platforms, especially macOS (CoreGraphics/Metal rendering).
2. **Scaling quality.** Qt's `QPixmap::scaled()` is fast but not publication-grade. For 50% resolution, bilinear should be adequate. Custom resolutions must maintain aspect ratio (padding or letterboxing?) or accept distortion.
3. **File permissions.** Output directory may not be writable (mounted read-only in CI). Error handling must be clear.
4. **Path normalization.** Filenames in test scripts should not include `/` or `..` to prevent directory traversal. Reject or sanitize.
5. **Performance.** Capturing full resolution on every assertion slows tests. Keep explicit; don't auto-capture.

## 8. Out of scope

- Video/animated captures (only static PNG).
- Pixel-exact regression testing (CI artifacts are informational for now).
- Delta/diff imaging between runs.
- Audio waveform visualization.
