# tw/record — CONTRACT

Purpose: RecordingSession — capture from an AudioInput on a worker thread,
resample device rate → project rate, write per-armed-track WAV files,
advance the playhead via callback.

Public headers: recording_session.h.

Depends on: tw/core, tw/devices, tw/sinks, tw/sources (LinearResampler).
Forbidden: app headers — the app supplies startLocatorFrames in params and
receives positions via onPosition.

Invariants:
1. The record worker is the sole locator authority while active (the
   speaker checks locatorHeldElsewhere) and publishes
   startLocatorFrames + captured PROJECT-rate frames.
2. Worker thread: no Qt (THREADING.md rule 1); the progress dialog POLLS
   the query methods, it does not use the callbacks.
3. Files are written at the PROJECT rate regardless of device rate;
   duration reporting uses device-rate frames over wall-clock (the
   effective-rate diagnostic guards against wrong-rate captures).
4. Stop is graceful: files are finalized (or cleaned up on error) before
   isFinished() flips.

How to test: manual (Ctrl-R with an armed track); the WAV output is a plain
sinks-path file. No headless coverage yet.

Known debt: one file per track duplicates identical channel content; no
input monitoring path; CoreAudio input returns silence (placeholder).
