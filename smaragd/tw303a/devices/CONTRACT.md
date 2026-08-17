# tw/devices — CONTRACT

Purpose: platform I/O. AudioBackend (callback-pull output) and AudioInput
(capture) plus the WASAPI/ALSA/CoreAudio/Null/Capture implementations; and,
since proposal 37 P7a, the MIDI half — MidiOutput/MidiInput plus the
WinMM/CoreMIDI/ALSA-sequencer/Capture/Null implementations and MidiOutScheduler,
the one thread that puts MIDI bytes on the wire at their due time.

Public headers: audio_backend.h, audio_input.h, null_backend.h,
capture_backend.h, midi_output.h, midi_input.h, midi_out_scheduler.h,
capture_midi.h, null_midi.h, and the per-platform AUDIO backend headers.
Platform *_input.h and every platform MIDI header (winmm_midi.h,
coremidi_midi.h, alsa_seq_midi.h) are PRIVATE (src/).

Depends on: tw/core. Platform SDKs (ole32/avrt/winmm/ALSA/CoreAudio/CoreMIDI)
are PRIVATE link deps; QBX_* backend defines are PRIVATE compile definitions.
Forbidden: tw/graph and above — a backend moves buffers, it does not know
components. MIDI knows nothing of tw/events either: the wire carries BYTES, and
the model-to-bytes conversion belongs to the app's pump (proposal 37 P7b).

Invariants:
1. The render callback runs on the BACKEND'S thread: THREADING.md rule 1
   applies to everything reachable from it.
2. openDevice/startOutput/stopOutput/closeDevice are blocking control-plane
   calls (UI/worker thread), never called from the callback.
3. stopOutput() blocks until the callback thread has exited — callers rely
   on this for teardown ordering.
4. Device ids are backend-native strings; "default"/empty = system default.
5. createAudioBackend()/createAudioInput() choose the platform impl via the
   QBX_* defines; adding a backend touches this module + CMake only.
6. SMARAGD_AUDIO_BACKEND (capture|null|default) outranks the platform choice
   in createAudioBackend(). It is read ONCE, inside twSpeaker's constructor —
   which is why the selection is an environment variable and not an argument:
   there is no call site between SApplication's constructor and the backend.
   A --test-case run sets it to `capture` before SApplication exists, unless it
   is already set (an explicit setting always wins, in both directions).
7. CaptureBackend pumps the render callback on a REAL-TIME PACED clock
   (sleep-until-deadline, SMARAGD_CAPTURE_SPEED multiplies it) and never waits
   for audio to become available. A virtual clock would remove exactly the
   pressure the playback tests hunt — the callback arriving before the page is
   ready. Falling behind re-anchors the deadline and logs once; it never
   sprints to catch up.
8. CaptureBackend keeps EVERY frame of a block, including the tail after a
   short pull, because the device would have consumed the whole block. Dropping
   it would compress the recorded timeline and silently move every later frame,
   which is precisely what a position decode exists to catch. The recording is
   cleared by startOutput() (so captured frame 0 is the current playback
   session's first frame) and SURVIVES closeDevice() (a case dumps it after
   stopping). It is capped at kMaxCaptureSeconds, and frames past the cap are
   counted in droppedFrames rather than silently discarded.
9. CaptureBackend ALSO logs {hostTimeNs, firstFrame} per block it keeps
   (capturedBlockLog / frameAtHostTime, piecewise linear). That log is the
   INDEPENDENT clock a MIDI-out assertion is measured against (proposal 37 D6,
   review #12): the MIDI capture port records host times only, so the map from
   host time to project frame comes from the audio pump, which knows nothing of
   the pump under test. It is stamped with MidiOutScheduler::hostNowNs() — the
   SAME steady clock — and cleared with the recording. A block past the cap is
   not stamped: a log entry for a frame the recording does not hold would map
   host times onto an index no decoder can reach. SMARAGD_CAPTURE_SPEED != 1
   still works (the log is empirical), but it changes what a project frame means
   in wall-clock terms, so MIDI-out cases run at 1.0.

--- MIDI (proposal 37 P7a) ---

10. MIDI out is emitted at PLAY time by MidiOutScheduler's thread, never at
    freeze time. This is the metering lesson (proposal 34) verbatim: pages are
    frozen ~1.4 s ahead of the playhead and by renders that have no playhead at
    all, so anything derived at freeze time describes the future. The app's pump
    (P7b) reads the playhead and enqueues; a render emits nothing.
11. enqueue() is SINGLE-PRODUCER through a lock-free ring; that producer is the
    app's main-thread pump. flush()/panic()/start()/stop() are control-plane
    calls from the same thread. A second producer corrupts the ring silently.
12. The scheduler thread and every device thread are Qt-FREE and joined
    directly (THREADING.md rule 1). Nothing in this module may emit a signal,
    touch a QObject, or own a thread_local with a non-trivial destructor.
13. Timing granularity is ~1 ms, and on Windows that needs a HIGH-RESOLUTION
    waitable timer, not timeBeginPeriod(1) alone: measured on this repo's box, a
    condition_variable wait rounded up to the 15.6 ms system tick with the
    period request in force. When a backend supportsTimestamps() (CoreMIDI,
    ALSA-seq queues) the scheduler hands off early and the driver does the
    pacing instead.
14. flush() DISCARDS the queue rather than pushing it out — at a stop or a
    locate the queued future belongs to a playhead that no longer exists, and a
    note-on that escaped afterwards would be a stuck note. stop() discards for
    the same reason. panic() flushes first, then sends CC64=0 + CC123=0 per
    selected channel.
15. Overload is BOUNDED and COUNTED at two places, never queued without limit:
    the ring refuses when full, and the sender caps its pending list at
    kRingSlots by dropping the furthest-future messages. Both count in
    dropped(); a message longer than kMaxMessageBytes is refused, never
    truncated.
16. The CAPTURE MIDI port records {hostTimeNs, port, bytes} and nothing else —
    specifically NOT the due time it was asked for, because the difference
    between the two IS the measurement. It reports supportsTimestamps() ==
    false on purpose: reporting true would make the scheduler hand off early
    and the recorded instant would become the handoff instant.
17. createMidiOutput()/createMidiInput() select by SMARAGD_MIDI_BACKEND
    (winmm|coremidi|alsaseq|capture|null|default) ahead of the platform, exactly
    like SMARAGD_AUDIO_BACKEND; an unknown value warns and falls back to the
    platform, never to a null pointer. A --test-case run is intended to default
    to `capture` (set in main.cpp before SApplication exists, unless it is
    already set — the app half is P7b). Unlike the audio variable it is read per
    call, because a MidiOutput is minted where a caller CAN pass a name, hence
    the explicit createMidiOutput(backend) overload.
18. Virtual ports exist on CoreMIDI (MIDISourceCreate/MIDIDestinationCreate) and
    ALSA-seq (every application port is one); WinMM has no such concept and
    createVirtualPort() returns FALSE there — a UI must gate on that return, not
    on the platform. loopMIDI-style loopback drivers appear as ordinary devices.

How to test: WASAPI is the only regularly exercised backend (manual GUI
playback); Null backend keeps headless/CI paths honest. devices/tools/
asio_probe (Windows, needs the drop-in ASIO SDK — proposal 35) triages an
installed ASIO driver end to end without starting the app. The CAPTURE backend is
what makes the playback path assertable at all — it is the headless default, so
every qxa case with a <toggle-playback> runs through it, and
playback_start_after_edit_position decodes its recording position by position.

devices_midi_test (ctest, RUN_SERIAL) is the MIDI gate: a capture round trip
through the scheduler that reports the measured max |sent − due| and asserts
5 ms, shutdown under a full queue, the frameAtHostTime map over a synthetic
log, the backend selection, and the null port's never-fails contract. It
measures the MACHINE as much as the code, like twlog_test — confirm the box is
idle before reading a failure as a regression.

Known debt: WASAPI shared-mode only; ALSA untested since the refactor;
PipeWire/Pulse/JACK placeholders; input enumeration shows only defaults.
CaptureBackend has no unit test of its own (its pacing is a wall-clock
property); it is covered end to end by the qxa cases that play.
MIDI: the CoreMIDI and ALSA-sequencer backends are UNVERIFIED — written and
reviewed on Windows, compiled and run nowhere in the P7a gate. WinMM sysex OUT
blocks the sender thread until the driver releases the header (rare, bounded,
and the alternative was a completion queue for a path nothing uses yet); sysex
IN (MIM_LONGDATA) is not implemented at all. Neither MidiInput nor the capture
MIDI input has a consumer until proposal 37 P8. Send jitter against real
hardware, driver timestamps, and virtual-port creation on Windows (which needs a
loopback driver) are not gated by anything.
