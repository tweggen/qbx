# tw/record — CONTRACT

Purpose: RecordingSession — capture from an AudioInput on a worker thread,
resample device rate → project rate, write per-armed-track WAV files,
advance the playhead via callback.

Public headers: recording_session.h.

Depends on: tw/core, tw/devices, tw/sinks, tw/sources (LinearResampler).
Forbidden: app headers — the app supplies startLocatorFrames in params and
receives positions via onPosition.

Invariants:
1. The record worker is the locator authority only while NOTHING IS
   AUDIBLE — from capture start until the monitoring playback is running
   (the speaker checks locatorHeldElsewhere, which the app now gates on
   that window rather than on "recording at all"). It publishes
   startLocatorFrames + captured PROJECT-rate frames; once the monitor is
   up the SPEAKER publishes the position it is actually delivering.
   Why: startOutput() returns before the device starts (twSpeaker defers
   it until the readahead is primed, ~1.4 s per page), so a playhead
   driven by captured frames ran ahead of anything the user could hear for
   the whole take — and then drifted further, because a capture-frame
   count is only a clock if the capture clock is right. A measured take
   delivered 8.6 % more frames than wall clock (see invariant 3's
   diagnostic), which is a second reason not to steer the timeline with
   it.
2. Worker thread: no Qt (THREADING.md rule 1); the progress dialog POLLS
   the query methods, it does not use the callbacks.
3. Files are written at the PROJECT rate regardless of device rate;
   duration reporting uses device-rate frames over wall-clock (the
   effective-rate diagnostic guards against wrong-rate captures).
4. Stop is graceful: files are finalized (or cleaned up on error) before
   isFinished() flips.

How to test: manual (Ctrl-R with an armed track); the WAV output is a plain
sinks-path file. No headless coverage yet.

Known debt: one file per track duplicates identical channel content — note
this is about the INPUT side and is unaffected by proposal 36, which widened
the graph and the output sink; RecordingParams::channels is still hard-coded 2
in SMainWindow and SObject::recordingChannels_ is live in the UI but never
serialized (proposal 36 §7 trap 2), so a recording does not follow the
project's width. No input monitoring path; CoreAudio input returns silence
(placeholder).
