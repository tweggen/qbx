# Threading Model: Test Protocol

## Critical Scenarios (Previously Broken)

Test the exact scenarios that showed corruption/glitches before the double-buffer fix.

---

## Test 1: Volume Change During Playback

**Scenario:** User reported waveform corruption when changing volume slider while audio plays on the cut.

**Setup:**
1. Load audio file into a track
2. Arrange the clip on the timeline
3. Make sure the track is visible in the arrange window

**Test Steps:**
```
1. Press Play → audio should start playing
2. WHILE PLAYING, move the volume slider of the PLAYING track
   - Start at 0.0 dB
   - Drag to -12 dB
   - Drag back to 0 dB
   - Rapid back-and-forth: 0 → -20 → +3 → 0 (2-3 seconds)
3. Observe the waveform preview as you drag
4. Continue playback for 5 more seconds after done dragging
```

**Expected Result:**
- ✅ Waveform preview updates smoothly
- ✅ Preview shows correct amplitude at each volume level
- ✅ No corruption (garbled pixels, noise, holes)
- ✅ No glitches in playback audio
- ✅ No crashes

**Previous Failure:**
- ❌ Waveform preview showed corrupted/garbled display
- ❌ Same audio file appeared different depending on volume
- ❌ Preview looked like noise floor at some positions

---

## Test 2: Window Drag During Playback

**Scenario:** User reported corruption when dragging clip boundaries while audio plays.

**Setup:**
1. Load audio file into a track
2. Arrange it in the middle of the timeline (bars 5-10)
3. Ensure playback cursor is positioned to play through it

**Test Steps:**
```
1. Position playback cursor at bar 4 (before the clip)
2. Press Play → cursor should reach the clip and start playing it
3. WHILE PLAYING (cursor ON the clip), grab the right edge of the clip and drag it:
   - Drag right (extend clip): 10 → 11 → 12 bars
   - Drag left (shorten clip): 12 → 8 → 6 bars
   - Rapid changes: 6 → 14 → 7 → 10 bars (back and forth)
4. Continue playback after done dragging (5 more seconds)
5. Repeat test dragging the LEFT edge:
   - Drag left (move start earlier): bar 5 → bar 4 → bar 3
   - Drag right (move start later): bar 3 → bar 7 → bar 5
```

**Expected Result:**
- ✅ Waveform preview updates as you drag
- ✅ Preview reflects the new window position (shows different part of audio)
- ✅ No visual corruption
- ✅ Playback continues smoothly
- ✅ Audio output is correct (no glitches, pops, corruption)
- ✅ No crashes

**Previous Failure:**
- ❌ Preview showed corrupted waveform when dragging boundaries
- ❌ Same audio appeared with different shapes at different windows
- ❌ Playback audio glitched/corrupted

---

## Test 3: Grain Parameter Changes During Playback

**Scenario:** Stretch/pitch changes (trigger reader rebuild) during playback.

**Setup:**
1. Load audio file
2. Set up to play it

**Test Steps:**
```
1. Start playback (cursor approaching the clip)
2. WHILE PLAYING, if there's a stretch/pitch control, adjust it:
   - Change stretch: 1.0x → 1.5x → 0.8x → 1.0x
   - Or change pitch: 0 cents → +12 → -12 → 0
3. Listen for artifacts
4. Watch for visual corruption
```

**Expected Result:**
- ✅ Parameters update smoothly
- ✅ Audio rendering reflects changes (pitch/stretch audible if changed)
- ✅ No pops, clicks, or corrupted audio
- ✅ No visual glitches
- ✅ No crashes

---

## Test 4: Stress Test (All Concurrent)

**Scenario:** Maximum concurrent modification stress.

**Setup:**
1. Load multiple audio files
2. Arrange them in timeline
3. Set up to play multiple tracks

**Test Steps:**
```
1. Start playback with multiple tracks playing
2. For 30 seconds, CONTINUOUSLY:
   - Change one track's volume slider
   - Drag another track's window boundaries
   - Change a third track's stretch/pitch
   - Rapid back-and-forth changes
3. Listen and watch for any issues
4. Let playback continue another 10 seconds after edits stop
```

**Expected Result:**
- ✅ All operations complete without blocking
- ✅ Audio playback is smooth and correct
- ✅ Waveform previews update correctly
- ✅ No corruption, glitches, or crashes

---

## Test 5: Edge Case - Playback Cursor ON Edited Clip

**Scenario:** Most stressful case: audio thread actively rendering the EXACT clip being edited.

**Setup:**
1. One track with audio
2. Clip positioned bars 5-10

**Test Steps:**
```
1. Press Play at bar 4
2. Wait for cursor to reach bar 6 (middle of clip, actively playing)
3. AT THIS MOMENT (cursor actively on the clip):
   - Drag left edge: bar 5 → bar 4 (shorten clip start)
   - Drag right edge: bar 10 → bar 11 (extend clip end)
   - Change volume: 0 → -15 dB
   - Change stretch: 1.0 → 1.2x
4. Do all of these simultaneously/rapidly while cursor is ON the clip
5. Watch the playback cursor move through the modified clip
```

**Expected Result:**
- ✅ Modifications apply mid-playback
- ✅ Audio seamlessly reflects changes
- ✅ No crashes, no corruption, no glitches
- ✅ Playback continues correctly

**This is the HARDEST case** — if this works, the model is solid.

---

## Recording Results

For each test, record:

```
Test 1: Volume Change During Playback
  Visual corruption?     [✅ No / ❌ Yes - describe]
  Audio glitches?        [✅ No / ❌ Yes - describe]
  Crashed?               [✅ No / ❌ Yes]
  Notes: _________________________________

Test 2: Window Drag During Playback
  Visual corruption?     [✅ No / ❌ Yes - describe]
  Audio glitches?        [✅ No / ❌ Yes - describe]
  Crashed?               [✅ No / ❌ Yes]
  Notes: _________________________________

Test 3: Grain Parameters During Playback
  Visual corruption?     [✅ No / ❌ Yes - describe]
  Audio glitches?        [✅ No / ❌ Yes - describe]
  Crashed?               [✅ No / ❌ Yes]
  Notes: _________________________________

Test 4: Stress Test (All Concurrent)
  Visual corruption?     [✅ No / ❌ Yes - describe]
  Audio glitches?        [✅ No / ❌ Yes - describe]
  Crashed?               [✅ No / ❌ Yes]
  Notes: _________________________________

Test 5: Cursor ON Edited Clip (HARDEST)
  Visual corruption?     [✅ No / ❌ Yes - describe]
  Audio glitches?        [✅ No / ❌ Yes - describe]
  Crashed?               [✅ No / ❌ Yes]
  Notes: _________________________________

OVERALL: [✅ Rock Solid / ⚠️ Some Issues / ❌ Major Problems]
```

---

## What Each Result Means

### ✅ All Tests Pass
- Double-buffer model is working correctly
- No remaining race conditions in reader path
- Foundation is rock solid for building on

### ⚠️ Some Tests Fail
- Specific scenarios still have races
- Likely in a different part of the system (not reader state)
- Need to audit that specific path

### ❌ Major Problems
- Threading model didn't fully solve the issue
- May need to double-buffer other state (preview cache, etc.)
- Return to architecture analysis

---

## Instructions

1. **Launch the app:** `open build/bin/smaragd.app`
2. **Load test audio:** Use File → Open to load an audio file
3. **Run Test 1-5** in order
4. **Record results** for each
5. **Share results** with description of what you see

The double-buffer model should make ALL of these work smoothly.
