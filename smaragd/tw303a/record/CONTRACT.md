# tw/record — CONTRACT

Purpose: the CAPTURE BRIDGE — one input pump, three sinks (proposal 21 L3a,
design D7) — and `RecordingSession`, the transport-scoped consumer of it that
the app starts, polls and reads created files from.

Public headers: capture_bridge.h, recording_session.h.

Depends on: tw/core, tw/devices, tw/sinks, tw/sources (`twGrowingCaptureSource`,
and `LinearResampler` which lives privately in `record/src/`).
Forbidden: app headers — the app supplies `startLocatorFrames` in params and
receives positions via `onPosition`. Also **tw/playback**: the live-lane sink is
exposed as `CaptureBridge::pullLive()` in `twLiveInputSource::pull()`'s exact
shape rather than as a subclass of it, precisely so this module does not have to
grow an edge to the module that declares that interface (the app's adapter is
ten lines).

## The shape

```
device capture thread ──> AudioInput's SPSC ring (L0)
                            │   ONE consumer
                            ▼
                     BRIDGE THREAD  ──┬──> live-lane ring   (SPSC; pullLive())
                                      ├──> twGrowingCaptureSource   THE PAGES
                                      └──(not here)
                                              ▲
                                   WAV THREAD ─┘ reads the pages BY POSITION,
                                                writes N files, may be LATE
```

Invariants:

1. **The bridge thread is the input ring's ONLY consumer**, and the device's
   capture thread its only producer. `pullLive()` has exactly one consumer too
   (the pump). Two consumers on either ring is a data race, not a slow path.

2. **The WAV sink can never stall the ring.** It does not run on the bridge
   thread at all. A writer that falls behind costs a BACKLOG — `wavLate`, a
   high-water mark in frames — and nothing else: the ring keeps draining, the
   pages keep growing, `ringOverruns` stays 0. Anything that puts a
   `write()` call on the bridge thread breaks this and the gate says so
   (`record_bridge_test`, the deliberately slow writer).

3. **The pages are the record; the file is a view of them that may be late.**
   At stop, `finalizeFromPages()` completes every file out of the growing
   capture source, and those frames are counted in `wavFinalized`. It is the
   ORDINARY drain call, made after `captureEnded_` — there is no second write
   path that could disagree with the first.

4. **Stop order is fixed and each step depends on the one before it:**
   `stopCapture()` (the device stops producing, so the ring has a final size) →
   the bridge thread drains the ring to empty and exits, publishing
   `captureEnded_` → the WAV thread finalises → close the writers → close the
   device. `stop()` BLOCKS; `RecordingSession::requestStop()` must not, which is
   why the session keeps a thread whose only job is to call it.

5. **Files are written at the PROJECT rate** regardless of device rate. The
   resampler (`record/src/linear_resampler.h`) sits between the ring pop and
   ALL THREE sinks, so the pages and the files can never disagree about rate,
   and it is not called at all when the rates already match — which is what
   makes "pages == WAV == the input file, sample for sample" a claim the gate
   can and does make.

6. **Steady state on the bridge thread is allocation-free**, except for ONE
   growing-source chunk at a chunk boundary (512 KB every 1.37 s for stereo at
   48 kHz). The pop scratch, the resampler's output vector and the per-sink
   interleave scratch are all sized once in `start()`.
   `twGrowingCaptureSource::reserveThrough()` exists for a caller that wants
   even the chunk paid up front.

7. **Nothing here touches Qt** (THREADING.md rule 1) — three threads
   (bridge, WAV, session) and none of them may. The progress dialog POLLS
   `RecordingSession`'s query methods; `onPosition` is a realtime atomic store
   made on the BRIDGE thread; `onProgress`/`onComplete` come from the session
   thread.

8. **There is no poll loop.** The old capture loop slept 1 ms whenever the
   device had nothing; the bridge thread waits on a condition variable for half
   a device block period (1–10 ms, block-paced) and the session thread waits
   until stop is requested, waking on a 100 ms timeout only to emit progress.

9. **A CAPTURE SEGMENT is what the pages and the files belong to** (proposal
   21 L3b). A bridge started with `capturePages = false` is a MONITOR-only
   pump: it feeds the live ring and holds no growing source at all, because
   the pages are the record of a RECORDING and growing them for a monitoring
   session would leak ~370 KB/s of the user's RAM for audio nobody asked to
   keep. `beginCapture(sinks)` then opens a segment ON A RUNNING BRIDGE — a
   fresh growing source, fresh writers, and the device, its capture thread and
   its ring untouched, which is what makes a record start while monitoring not
   gap the monitored signal. `endCapture()` closes it: it clears `capturing_`
   under `m_` (so no append can follow — a fence, not a hint), makes the
   ORDINARY drain from the calling thread, and closes the writers. `source()`
   stays valid and readable afterwards.

9a. **TWO MUTEXES, AND THE ORDER IS ALWAYS `sinkM_` BEFORE `m_`.** `m_` is the
   short one the BRIDGE thread takes (its segment snapshot, its append, its
   CV wait); `sinkM_` is the long one the WAV thread holds across a whole
   drain, so a control-plane call that must not interleave with a write takes
   it first and then `m_`. Putting the drain under `m_` instead would let a
   large backlog block the bridge thread for the length of a file write, which
   is what invariant 2 forbids.

9b. **`liveEnabled` gates the live-ring push.** A recording with monitoring OFF
   has nothing popping the ring; leaving the push on would fill it once and
   then count EVERY frame of the take as a `liveOverruns` — a phantom counter
   on a perfectly healthy take.

9c. **`captureStartHostNs()` is the input half of the placement anchor** (design
   D6). It is the pop time of the segment's first batch MINUS that batch's own
   duration, because a device hands over audio it has already recorded. The app
   maps it through the engine clock to get `P0`; nothing here knows what a
   project frame is.

10. **`channelMask` is per SINK, not per capture.** One capture feeds several
   armed tracks of different widths (`readInterleaved` applies the mask on the
   way out), so a second copy of the audio is never made. Mask 0 == every
   channel.

11. The session's playhead publication is unchanged: `startLocatorFrames` plus
   the growing source's frontier, i.e. PROJECT-rate frames captured so far.

How to test: `ctest -R record_bridge_test` — the growing source as a data
structure, then the whole fan-out over a paced `FileAudioInput`
(`SMARAGD_AUDIO_INPUT_BACKEND=file:<wav>`): pages == WAV == the input file
SAMPLE-EXACT at 2 and 6 channels, the stalled-writer backpressure claim
(`ringOverruns == 0` while `wavLate > 0`, file still exact), the live-lane pull
(concurrent and after the fact), the per-sink channel mask, and
`RecordingSession` end to end. It drives a REAL-TIME PACED device, so it is
`RUN_SERIAL` and measures the machine as well as the code; it is threaded, so
loop it (>= 20 iterations) when touching the bridge.

Known debt:
- The bridge thread's wake is a block-paced condition-variable TIMEOUT, not an
  event: `AudioInput` has no "wake me when frames arrive" ABI. Bounded by the
  ring's 16-block depth, so it cannot overrun; it does add up to ~10 ms to the
  live lane's latency, which L1b's monitor-latency budget has to carry.
- One file per track still duplicates identical channel content when two armed
  tracks take the same mask.
- The recording is not PLACED by this module — the growing capture source is
  handed on; `SRecordingContent`, the frontier-drawing clip and the placement
  conversion are L3b (`main/objects/wave`, `main/shell`).
- **`RecordingSession` no longer has an app consumer** (proposal 21 L3b). The
  app drives `CaptureBridge` directly through `SAudioRecorder`, which names the
  files, owns the transport edge and does the placement — everything the
  session used to wrap. The class is kept because `record_bridge_test` drives
  it end to end and because deleting it is not this phase's risk to take;
  whoever next touches this module should consider retiring it.
- CoreAudio input returns silence (placeholder, L0 debt); real capture
  hardware, ALSA and CoreAudio are not gated anywhere.
