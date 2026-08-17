# tw/devices — CONTRACT

Purpose: platform I/O. AudioBackend (callback-pull output) and AudioInput
(capture) plus the WASAPI/ALSA/CoreAudio/Null/Capture implementations; and,
since proposal 37 P7a, the MIDI half — MidiOutput/MidiInput plus the
WinMM/CoreMIDI/ALSA-sequencer/Capture/Null implementations and MidiOutScheduler,
the one thread that puts MIDI bytes on the wire at their due time.

Public headers: audio_backend.h, audio_input.h, audio_ring.h, null_backend.h,
capture_backend.h, midi_output.h, midi_input.h, midi_out_scheduler.h,
capture_midi.h, null_midi.h, and the per-platform AUDIO backend headers.
Platform *_input.h, file_input.h, precise_waiter.h and every platform MIDI
header (winmm_midi.h, coremidi_midi.h, alsa_seq_midi.h) are PRIVATE (src/).
audio_ring.h is public because the ring's contract IS the input contract and
because a gate has to be able to test the class directly.

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

--- INPUT (proposal 21 L0) ---

19. EVERY capture device has a THREAD and a RING, and read() is a POP.
    The thread waits on the device's own event (WASAPI: a client initialised
    with AUDCLNT_STREAMFLAGS_EVENTCALLBACK + SetEventHandle; ALSA:
    snd_pcm_wait; the file backend: a paced high-resolution timer; CoreAudio:
    the AVAudioEngine tap, which is already such a thread and is not ours to
    create) and pushes the WHOLE packet into an SPSC AudioRing. It takes no
    lock — stopCapture() joins it while holding the object's mutex, so a
    capture thread that wanted that mutex would deadlock the stop — and the
    handles it uses are created before it starts and released after it is
    joined.
    What this replaced: read() WAS the device poll, and WASAPIInput::read()
    copied min(packet, caller's buffer) and then released the WHOLE packet, so
    every frame past the caller's buffer was DROPPED and the recorded timeline
    silently compressed (design 21 §1 F7). Separating "what the device gave us"
    from "what the caller asked for" is the only shape in which that cannot
    happen; the tail now costs latency, never audio.
20. The ring never overwrites unread frames. A push that does not fit takes
    what fits and COUNTS the rest in overrunFrames(): in SPSC the consumer may
    be mid-copy out of exactly the oldest frames, so "drop the oldest" is a
    data race, not a policy. A dropped frame is a number a test can assert on;
    a corrupted one is not. A SHORT pop counts an underrun, an EMPTY one does
    not — an idle device with nothing to give is the normal case, not a hole.
21. ONE producer, ONE consumer, and reset()/clear() are CONTROL PLANE. The
    consumer is whichever thread calls read() — RecordingSession's worker
    today, proposal 21 L1a's pump later, never both at once. openDevice /
    startCapture / stopCapture / closeDevice come from one thread, as they
    always have.
22. SMARAGD_AUDIO_INPUT_BACKEND (file:<wav> | null | default) outranks the
    platform choice in createAudioInput(), exactly as SMARAGD_AUDIO_BACKEND
    does for output — but it is read PER CALL, because unlike the speaker's
    backend an AudioInput is minted at several call sites. An unknown value
    warns and falls back to the platform, never to a null pointer. A
    --test-case run defaults it to `null` (main.cpp, unless already set): a
    headless suite must not open the developer's microphone, and what a real
    input delivers is whatever is in the room and therefore not assertable.
23. FileAudioInput is a REAL device, paced in REAL TIME, at MMCSS priority.
    Blocks of 1024
    frames, due at t0 + (i+1) * period on MidiOutScheduler::hostNowNs() — the
    same steady clock CaptureBackend stamps its blocks with, so a case can map
    what it sent onto what it heard — through the same kind of high-resolution
    waitable timer inv. 13 exists for. The reason is inv. 7's verbatim: a clock
    that handed the whole file over at once would remove exactly the pressure
    (the consumer arriving before the frames do) that a live-input case is
    looking for. Block i is delivered one period AFTER its start, because a
    device cannot hand over audio it has not recorded yet. Its WAV reader is
    hand-rolled ON PURPOSE: libsndfile lives behind tw_sources/tw_sinks, and
    reaching for it here would make the platform I/O layer depend on the codec
    stack for the sake of a fixture. Its capture thread takes MMCSS "Pro Audio"
    exactly as the WASAPI RENDER thread does — it is standing in for a device,
    and without the promotion an ordinary desktop scheduling hiccup shows up as
    delivery jitter the real thing would not have had (measured: roughly double,
    with outliers past 3 ms). It also keeps a per-block PUSH-TIME log, the file
    backend's analogue of inv. 9's block log and for the same reason: pacing is
    a wall-clock property, so the only way to hold it to a number is to record
    when delivery actually happened.
24. PreciseWaiter (src/) is MidiOutScheduler's wait, extracted for the second
    paced thread. The scheduler still owns its copy: it is on the MIDI timing
    gate, and re-pointing a green measured path at a new class buys nothing.
    This is the form a future consolidation takes, not that consolidation.

How to test: WASAPI is the only regularly exercised backend (manual GUI
playback); Null backend keeps headless/CI paths honest. devices/tools/
asio_probe (Windows, needs the drop-in ASIO SDK — proposal 35) triages an
installed ASIO driver end to end without starting the app. The CAPTURE backend is
what makes the playback path assertable at all — it is the headless default, so
every qxa case with a <toggle-playback> runs through it, and
playback_start_after_edit_position decodes its recording position by position.

devices_input_test (ctest, RUN_SERIAL) is the INPUT gate (proposal 21 L0 AC1):
the ring under a producer three times the consumer's pop size (the tail-drop
regression, at the level the fix lives), FileAudioInput replaying a 2 s
position-coded WAV it generates itself — every frame exactly once, in order,
sample-exact against the file, every block DELIVERED within 2 ms of its paced
due time (MEASURED 0.68-0.91 ms max over six runs on this repo's idle box) and
observed by a polling consumer within 15 ms (measured 1.1-1.9 ms) — the capture
MIDI input's injection order and stamps, and the backend selection including
`null`.
The two numbers are separate ON PURPOSE. The device's own schedule is what this
module promises and is held to 2 ms; what a polling reader SEES additionally
contains the ring hop and the reader thread's own scheduling, and a desktop
deschedules an ordinary thread for a few milliseconds now and then — measured at
3.4 ms once in five runs taken straight after a full ctest sweep, while the
device-side number stayed inside 2 ms. Asserting the consumer's number tightly
would gate the machine rather than the code. (The capture thread runs at MMCSS
"Pro Audio", like the WASAPI render thread; without it the device-side jitter is
roughly double.) Like devices_midi_test this measures the MACHINE as much as the
code.

devices_midi_test (ctest, RUN_SERIAL) is the MIDI gate: a capture round trip
through the scheduler that reports the measured max |sent − due| and asserts
5 ms, shutdown under a full queue, the frameAtHostTime map over a synthetic
log, the backend selection, and the null port's never-fails contract. It
measures the MACHINE as much as the code, like twlog_test — confirm the box is
idle before reading a failure as a regression.

20. **The MIDI input device thread writes ONE RING PER CONSUMER, and the
    fan-out OWNS them for its whole life** (proposal 21 L2, design D8).
    `MidiInFanout` holds a FIXED array of `Sink`s; acquiring one flips an
    atomic flag, releasing it flips it back, and the device thread only ever
    touches memory the fan-out still owns. A consumer that registered its own
    ring would have to unregister it and then prove the device thread was not
    inside a `push()` on it, and that has no lock-free answer. A released sink
    is inert and reusable, and `acquire()` CLEARS it before raising the flag —
    the producer does not push into an inactive sink, so nothing can arrive
    between the clear and the store.

21. **MIDI-THRU has its own IMMEDIATE ring on `MidiOutScheduler`, and it is not
    `enqueue()`** (design D8). `sendImmediate()` is a second SPSC ring whose
    producer is a MIDI INPUT DEVICE THREAD and whose consumer is the sender;
    pushing thru bytes into `enqueue()`'s ring would corrupt its head silently,
    because that ring's single producer is the app's main-thread pump
    (inv. 11). It is drained FIRST in the sender loop, sends with due time 0
    (never handed to a driver queue, even on a backend that supports
    timestamps — thru has no future time to be scheduled at) and wakes the
    sender at once. It is deliberately OUTSIDE `flush()`'s discard: a flush
    drops a queued FUTURE, and a thru byte is a key being pressed now. ONE
    producer per scheduler is the caller's guarantee — `MidiInFanout::setThru`
    refuses a second target for a port and the app routes at most one input
    port to a given scheduler. Measured thru lag on an idle box: 0.011 –
    0.125 ms against design D8's 2 ms budget and AC5's 5 ms bound.

22. **The computer keyboard is a REAL `MidiInput` port and is NOT selected by
    `SMARAGD_MIDI_BACKEND`** (design D9). `createMidiInput("keyboard")` names
    `KeyboardMidiInput` explicitly; the environment variable chooses the SYSTEM
    MIDI implementation and the computer keyboard exists whatever it chooses,
    so it must neither replace the hardware backend nor be replaced by it. Its
    callback runs on the CALLING thread, exactly as `CaptureMidiInput::inject`
    does, which is what makes it a device thread as far as the fan-out is
    concerned. `createVirtualPort()` returns FALSE: there is nothing to create,
    and saying true would claim the capability inv. 18 means something else by.

23. **`twLiveEventSource::collect()` runs ON THE PUMP and CLAMPS A LATE EVENT
    TO OFFSET 0 — it never drops one** (design D4). Being late is the NORMAL
    case, not the exception: the pump renders ahead of the RT, so a byte that
    arrives while a block is being built is by construction older than that
    block, and clamping is what makes the latency the ring depth plus the lead
    rather than a whole extra block. An event mapped PAST the block is kept in
    `pending_` and emitted by the next collect, in order. The host-time
    mapping arrives as one virtual call (`twLiveFrameClock`) because
    tw/devices may not include tw/playback; the held-note table is what the ONE
    chase at live start re-attacks and what the all-notes-off flush empties.
    Every vector is sized on the MAIN thread, so the drain is allocation-free
    in the steady state. Two paths CAN allocate and both are named rather than
    hidden: deferring an event past the block grows a vector that is empty (and
    therefore free) whenever nothing is deferred, which is every ordinary
    block; and the chase set copies the controller maps, which allocates map
    nodes only once a CC / bend / program has actually been received - the same
    shape the sequenced feed'''s collect already has, and zero for the notes-only
    performance the gates measure.

Known debt (proposal 21 L2): `twLiveEventSource`'s deferred queue is UNBOUNDED.
An event whose mapped frame lands PAST the block is kept for the next collect,
which is right - but if the engine clock were to report `valid()` while the RT
had stopped stamping (a stalled device with the transport still flagged
playing), every arriving event would map far into the future and be re-deferred
forever. The queue is 24 bytes an entry and the trigger needs a stalled device,
so it is recorded rather than defended; the fix is a cap plus a drop counter,
next to the ring's own.

Known debt: the L0 capture threads are UNVERIFIED against hardware — the WASAPI
one is written and reviewed but was never run against a real microphone in the
L0 gate (nothing headless opens an input device), and the ALSA and CoreAudio
ones were written on Windows and compiled and run nowhere. The CoreAudio tap's
planar-to-interleaved conversion is new code fixing an old out-of-bounds read
(it copied frameCount * channels samples out of PLANE 0), and it too is
unverified. Everything that IS gated runs through FileAudioInput and the ring.
WASAPI shared-mode only; ALSA untested since the refactor;
PipeWire/Pulse/JACK placeholders; input enumeration shows only defaults.
CaptureBackend has no unit test of its own (its pacing is a wall-clock
property); it is covered end to end by the qxa cases that play.
MIDI: the CoreMIDI and ALSA-sequencer backends are UNVERIFIED — written and
reviewed on Windows, compiled and run nowhere in the P7a gate. WinMM sysex OUT
blocks the sender thread until the driver releases the header (rare, bounded,
and the alternative was a completion queue for a path nothing uses yet); sysex
IN (MIM_LONGDATA) is not implemented at all. Since proposal 21 L2 `MidiInput` HAS consumers - the live lane through
`MidiInFanout`, and MIDI-thru - but the recorder sink is declared and
unused until L4. Send jitter against real
hardware, driver timestamps, and virtual-port creation on Windows (which needs a
loopback driver) are not gated by anything.
