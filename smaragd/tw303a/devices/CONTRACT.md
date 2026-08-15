# tw/devices — CONTRACT

Purpose: platform audio I/O. AudioBackend (callback-pull output) and
AudioInput (capture) interfaces plus the WASAPI/ALSA/CoreAudio/Null/Capture
implementations.

Public headers: audio_backend.h, audio_input.h, null_backend.h,
capture_backend.h, and the per-platform backend headers. Platform *_input.h
headers are PRIVATE (src/).

Depends on: tw/core. Platform SDKs (ole32/avrt/ALSA/CoreAudio) are PRIVATE
link deps; QBX_* backend defines are PRIVATE compile definitions. Forbidden:
tw/graph and above — a backend moves buffers, it does not know components.

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

How to test: WASAPI is the only regularly exercised backend (manual GUI
playback); Null backend keeps headless/CI paths honest. devices/tools/
asio_probe (Windows, needs the drop-in ASIO SDK — proposal 35) triages an
installed ASIO driver end to end without starting the app. The CAPTURE backend is
what makes the playback path assertable at all — it is the headless default, so
every qxa case with a <toggle-playback> runs through it, and
playback_start_after_edit_position decodes its recording position by position.

Known debt: WASAPI shared-mode only; ALSA untested since the refactor;
PipeWire/Pulse/JACK placeholders; input enumeration shows only defaults.
CaptureBackend has no unit test of its own (its pacing is a wall-clock
property); it is covered end to end by the qxa cases that play.
