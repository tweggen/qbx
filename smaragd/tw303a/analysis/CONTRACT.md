# tw/analysis — CONTRACT

Purpose: acoustic metrics over WAV files (RMS energy, peak, optionally
region-scoped) for the test kit's assert actions.

Public headers: audio_analysis.h.

Depends on: tw/core; libsndfile PRIVATE. Forbidden: everything else.

Invariants:
1. analyzeWavFileRegion(start, count, channel): frames are per-channel;
   channel -1 = all channels folded.
2. Pure functions over files — no engine state, safe from any thread.

How to test: it IS the test instrument — assert-audio-energy/peak in the
qxa suite; the ramped fixture (tests/test_sawtooth.wav) gives every source
second a unique RMS.

Known debt: none tracked.
