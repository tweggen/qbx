# Render Start Offset Investigation: Why Exports Differ

## Summary

You rendered the same project twice without changing settings, but got **different starting positions**:
- **First render:** Started at sample ~224,126 (4.67 seconds into sawtooth)
- **Second render:** Started at sample 8 (0.00017 seconds into sawtooth)

**Root cause:** The render start position comes from a **project property that may not be persisted**, triggering **auto-generated default ranges** on each load.

---

## Call Chain: How Start Offset is Calculated

### 1. Render Dialog → RenderParams (srenderdialog.cpp:244-248)

```cpp
if (timeSelectionRadio_->isChecked() && project_) {
    params.extent = audio::RenderParams::Extent::TimeSelection;
    SProject::TimeRange selection = project_->getTimeSelection();
    params.startTimeSec = selection.startSeconds;
    params.endTimeSec = selection.endSeconds;
}
```

The render dialog reads the time selection from **`project_->getTimeSelection()`**.

---

### 2. SProject::getTimeSelection() (sproject.cpp:194-206)

```cpp
SProject::TimeRange SProject::getTimeSelection() const
{
    bool rangeValid = prop(SProjectProps::RangeValid, false).toBool();
    if (rangeValid) {
        // Read saved range from project properties
        double sampleRate = getSRate();
        double startSec = prop(SProjectProps::RangeStart, 0).toULongLong() / sampleRate;
        double endSec = prop(SProjectProps::RangeEnd, 0).toULongLong() / sampleRate;
        return { startSec, endSec };
    }
    return { 0.0, getDurationSeconds() };
}
```

This reads two project properties:
- **`RangeValid`** (boolean) — is a time range saved?
- **`RangeStart`** (uint64, in samples) — where does it start?
- **`RangeEnd`** (uint64, in samples) — where does it end?

**If RangeValid is false**, it returns the entire project duration (0.0 to end).

---

### 3. SMVActualView::loadRangeFromProject() (sstdmixerview.cpp:843-861)

**This is the critical function.** It's called when the UI view is initialized:

```cpp
void SMVActualView::loadRangeFromProject()
{
    SProject &p = smv_.model_->getProject();
    rangeValid_ = p.prop( SProjectProps::RangeValid, false ).toBool();
    rangeStart_ = (offset_t) p.prop( SProjectProps::RangeStart, (qulonglong) 0 ).toULongLong();
    rangeEnd_   = (offset_t) p.prop( SProjectProps::RangeEnd,   (qulonglong) 0 ).toULongLong();

    // **AUTO-GENERATE DEFAULT RANGE IF NONE SAVED**
    if (!rangeValid_) {
        STimeGridSpec tgs = smv_.getTimeGridSpec();
        double beatSec = tgs.getTimeGridWidth();
        int bpb = tgs.getEmphasizeGrids(0);
        int srate = smv_.model_->getProject().getSRate();

        offset_t barDurationSamples = (offset_t)(beatSec * bpb * srate);
        rangeStart_ = 4 * barDurationSamples;              // BAR 5 START
        rangeEnd_ = rangeStart_ + 4 * barDurationSamples;  // BAR 9 START
        rangeValid_ = true;
    }
}
```

**When `RangeValid` is false** (no saved range), it generates a default:
- Calculates `barDurationSamples = beatSec × beatsPerBar × sampleRate`
- Sets range to bars **5–8** (4 bars, starting at `4 × barDurationSamples`)

---

### 4. RenderSession::start() (render_session.cc:60)

```cpp
startOffsetSamples_ = static_cast<std::size_t>(params_.startTimeSec * sampleRate_);
```

The start offset is calculated as:
```
startOffsetSamples_ = startTimeSec (from getTimeSelection) × sampleRate
```

---

## Why Two Renders Produced Different Results

### Hypothesis: RangeValid Flag Toggled

| Render | RangeValid | Source | Start Position |
|--------|-----------|--------|-----------------|
| **First** | true (saved) | Project property | Sample ~224,126 |
| **Second** | false (lost) | Auto-generated default | Sample 8 |

Between the two renders, one of these likely happened:
1. The project was reloaded/reset, clearing the saved range
2. `clearTimeSelection()` was called accidentally
3. The properties_.remove() calls in `clearTimeSelection()` (line 219-220) affected state
4. A UI action (like clicking elsewhere) invalidated the range without saving

---

## Stability Issues

### Problem 1: Auto-Generation Calculates Differently Each Time

The auto-generated default in `loadRangeFromProject()` depends on:
```cpp
STimeGridSpec tgs = smv_.getTimeGridSpec();
double beatSec = tgs.getTimeGridWidth();
int bpb = tgs.getEmphasizeGrids(0);
```

If `timeGridSpec_` changes between invocations (e.g., user zooms, scrolls, or changes tempo), the auto-generated range will differ.

### Problem 2: No Persistence of User-Defined Ranges

- User drags markers in the timeline → `saveRangeToProject()` stores to project properties
- But if the project isn't **saved to disk**, those properties live only in memory
- On next load (or if view is recreated), `RangeValid` reads false → auto-generates default

### Problem 3: Render Dialog Doesn't Verify What It's Rendering

The render dialog doesn't show which time range will be used. User sets markers visually, but:
1. If properties weren't saved, render uses auto-generated defaults
2. Auto-generated defaults are always bars 5–8 (not bar 2)
3. User has no feedback that their markers were ignored

---

## Data Flow Diagram

```
User sets time markers in timeline
    ↓
SMVActualView::endRangeDrag()
    ↓
saveRangeToProject() → SProjectProps::RangeStart/End/Valid
    ↓
    ↓ (project file saved?)
    ↓
Next render: SRenderDialog::getParams()
    ↓
project_->getTimeSelection()
    ↓
SProject::getTimeSelection() reads RangeStart/End
    ↓
RenderParams.startTimeSec = RangeStart / sampleRate
    ↓
RenderSession::start()
    ↓
startOffsetSamples_ = startTimeSec × sampleRate
```

---

## Observations from Your Test Data

**First render.wav (96,000 samples, 2 sec):**
- Started at value 23264 (offset +56,032 from -32768)
- Sample position: ~224,126 into playback
- Time: ~4.669 seconds
- Pattern: **2-sample early start** (value appears only 2×, not 4×)

**Second render.wav (96,000 samples, 2 sec):**
- Started at value -32766 (offset +2 from -32768)
- Sample position: 8 into playback
- Time: ~0.00017 seconds
- Pattern: **Perfectly aligned** (first value appears 4×, correct quantization)

This suggests:
- First time: A saved range was used, but with a small alignment error
- Second time: Auto-generated default was used (bars 5–8 map to samples 0–96000 at 48 kHz)

If `barDurationSamples = 24,000` (0.5 sec per bar):
- Bar 5 start: `4 × 24,000 = 96,000` samples (2 seconds)
- That doesn't match sample 8...

So the second export might be using an even smaller bar size, or the "auto-generate" isn't being triggered at all.

---

## Recommendations

1. **Always check RangeValid before rendering**
   - Add a warning in the render dialog if no range is saved

2. **Make auto-generated defaults explicit**
   - Instead of silently generating bars 5–8, warn user: "No time selection found. Render entire project?"

3. **Verify time range is persisted**
   - Call `project_->save()` after `saveRangeToProject()` to ensure properties survive reload

4. **Add render preview**
   - Show in the render dialog: "Will render from X sec to Y sec" before starting

5. **Investigate the 2-sample offset in first render**
   - Check if there's an off-by-one error in how saved RangeStart is converted to samples
