# tw/analysis — CONTRACT

Purpose: acoustic metrics over WAV files (RMS energy, peak, optionally
region-scoped) for the test kit's assert actions.

Public headers: audio_analysis.h.

Depends on: tw/core; libsndfile PRIVATE. Forbidden: everything else.

Invariants:
1. analyzeWavFileRegion(start, count, channel): frames are per-channel;
   channel -1 = all channels pooled; count < 0 = to the end of the file.
   A channel index at or beyond the file's channel count is an ERROR — the
   alternative (selecting nothing) reports RMS 0 / peak 0, which is
   indistinguishable from a silent render.
2. Pure functions over files — no engine state, safe from any thread.
3. analyzeWavFile IS analyzeWavFileRegion(0, -1, channel). It used to be a
   separate path that hard-coded channel -1, which is how `channel=` came to be
   silently ignored by every whole-file assertion. Do not reintroduce a second
   whole-file path.
4. compareWavChannels reports rms(A), rms(B) and rms(A - B) from ONE pass.
   Levels and content are separate findings: two channels can hold the same
   level and different audio, and a level comparison calls those identical.

How to test: it IS the test instrument — assert-audio-energy/peak and
assert-channels-differ in the qxa suite. The ramped fixture
(tests/test_sawtooth.wav) gives every source second a unique RMS; the
asymmetric-channel fixture (tests/test_channels4.wav, written and re-verified
by tools/gen_channel_fixture.cc) gives every CHANNEL a unique RMS, which is
what makes "which channel did you measure?" answerable at all —
qxa.channel_assert_fixture.

Known debt: none tracked.
