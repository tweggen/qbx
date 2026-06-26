# Playback vs. Render Path Comparison

**Goal:** Systematically compare how container-backed cuts are handled during playback (working) vs. rendering (broken — second half only), to identify the root cause of the timing offset.

**Problem Statement:**
- Playback: Works correctly (with minor hickup entering indirect cut)
- Rendering: Only outputs second half of time range to non-zero audio
- Hypothesis: Rendering initialization differs from playback in a way that causes a 50% time offset

---

## Phase 1: Trace Playback Path

### Playback Initialization
1. User clicks Play → `SApplication::setPlaying(true)`
2. Speaker starts pulling audio via backend callback
3. **Q: What's the initial seek position?** Trace the call chain:
   - `twSpeaker::calcOutputTo()`
   - → `AudioEngine::pullFrame()`
   - → Does it seek? To what position?

### Playback: First Time Encountering Container-Backed Cut
Scenario: Playing reaches a SCut with a STrack parent (container-backed cut)

**Trace the call chain:**
1. `twTrackMix::calcOutputTo()` pulls from child cut
2. `lk->seekTo(offset)` — what offset?
3. `lk->getRootComponent()` — triggers what?
4. `SCut::getRootComponent()` → `ensureReader()` → `buildCapture_()`
5. `buildCapture_()` calls `renderObjectInto()`

**Key questions:**
- What is the value of `offset` passed to `seekTo()`?
- What values do `snap.startOffset` and `cutDuration` have?
- How much of the container is rendered into the capture?
- Does the capture include the correct time range?

### Playback: Reading From Container-Backed Cut
After capture is built:
1. How does `SCut::getRootComponent()` return the reader?
2. What does the reader do when `calcOutputTo()` is called?
3. Does the reader apply `startOffset` correctly?

---

## Phase 2: Trace Render Path

### Render Initialization
1. User selects File → Render
2. `SMainWindow::onRenderTriggered()` calls `SApplication::startRender(params)`
3. `RenderSession::start(synthOutput, params, sampleRate)`
   - Seeks: `synthOutput_->seekTo(startOffsetSamples_)`
   - Creates: `AudioEngine(synthOutput_, sampleRate_)`
   - Seeks: `audioEngine_->seekTo(startOffsetSamples_)`
4. Render loop: `audioEngine_->pullFrame()`

**Q: Are these seeks equivalent to playback?** Compare:
- Playback seek value vs. render `startOffsetSamples_`
- Does `AudioEngine::seekTo()` do the same thing in both paths?

### Render: First Frame Pulling Container-Backed Cut

**Trace:**
1. `AudioEngine::pullFrame()` → `pullStereoFrame()`
2. Pulls from `synthOutput_->linkOutput(0)` (the mixer)
3. Mixer pulls from all tracks, including container-backed cut's track
4. For Track 2 (container-backed): `twTrackMix::calcOutputTo()` iterates children
5. For each child cut:
   - `lk->seekTo(startOffset)` — **what is startOffset here?**
   - `lk->getRootComponent()` → triggers `buildCapture_()`
6. `buildCapture_()` calls:
   - `c.seekTo(0)` (newly added)
   - `renderObjectInto(c, buf.data(), n, ...)`

**Key questions:**
- In render, what is the value of `startOffset` passed to child `seekTo()`?
- Is it the same as in playback?
- When `buildCapture_()` is called during render, what is `n` (buffer size)?
- Is `n` calculated correctly? Should it include the full container duration or only what's needed?

---

## Phase 3: Identify Differences

### Comparison Table

| Aspect | Playback | Render | Same? |
|--------|----------|--------|-------|
| **Initial seek position** | ? | `startOffsetSamples_` | ? |
| **Seek sequence** | Continuous | One-time | ✗ |
| **When buildCapture_() called** | First frame through cut | First frame through cut | ✓ |
| **buildCapture_() seek target** | (no explicit seek before render) | `c.seekTo(0)` | ✗ |
| **Buffer size `n`** | N/A (streaming) | `max(container_duration, offset+cutDuration)` | ? |
| **How offset applied** | Reader offset during read | Buffer offset during read | ? |
| **Container child iteration** | Via twTrackMix state | Via renderObjectInto() loop | ✗ |

### Critical Questions
1. **Seek timing:** When does `buildCapture_()` get called in playback? Is it always before the first frame, or lazily on first access?
2. **Buffer calculation:** In `buildCapture_()`, is `n` the correct size? Should it be:
   - Full container duration? (current)
   - Only `snap.cutDuration`? (just the cut)
   - Only `snap.cutDuration - snap.startOffset`? (cut minus offset)
3. **renderObjectInto() semantics:** Does `renderObjectInto(obj, buf, len)` mean:
   - "Render obj's content from position 0 to len" (current assumption)
   - "Render obj's content from its current seek position for len samples" (alternate)
4. **Offset application:** After capture is built, how is `snap.startOffset` applied?
   - In the reader's `read(off, ...)` call?
   - Or should it be applied differently?

---

## Phase 4: Fix Strategy

Once differences are identified, the fix should:
1. Make render's initialization match playback's
2. Ensure `buildCapture_()` renders the correct time range
3. Ensure offsets are applied consistently
4. Verify the 50% offset symptom is resolved

### Testing
After fix:
- Render should include full time range (0–100%)
- Both playback and render should produce identical audio for the same region
- No timing offset in rendered output

---

## Implementation Notes

- **Files to trace:**
  - `sapplication.cpp`: `startRender()`
  - `render_session.cc`: `start()`, `renderThreadMain()`
  - `audio/audio_engine.cc`: `pullFrame()`, `pullStereoFrame()`
  - `tw303a/src/twtrackmix.cc`: `calcOutputTo()`
  - `scut.cpp`: `getRootComponent()`, `buildCapture_()`, `renderObjectInto()`

- **Debugger breakpoints to set:**
  - `SCut::buildCapture_()` entry (trace `n` value, container duration)
  - `renderObjectInto()` when called on container (trace buffer sizing, iteration)
  - `twTrackMix::calcOutputTo()` (trace seek positions passed to children)

---

## Related Issues

- Phase 5e: Unified page cache architecture (container-backed cuts)
- Previous attempts: Pre-warming captures, seeking fixes (partially helped but caused offset)
