# app/objects/wave — CONTRACT

Purpose: the sample object. SPlainWave (resident WAV via twWavInput, page-
cached preview), its inline renderer, the shared waveform drawing helper,
and add-sample / remove-sample actions.

Public headers: app/objects/wave/*.h

Depends on (engine): tw/core, tw/graph, tw/sources (+pages/schedule via
model base). App edges: per tools/check_layering.py.

Invariants:
1. getDuration() is project-rate frames from twWavInput::getLength()
   (viewAtRate) — a truncated file is clamped to real data at load; do not
   "fix" the duration to match the header.
2. getRandomSource() hands out the PROJECT-RATE view; consumers mint their
   own readers (never share the twWavInput cursor).
3. setWave() registers with the project's extern-file list; the destructor
   deregisters via its OWN project (not the app's current one).
4. Preview goes through the page cache with live fallback
   (getStraightPreview) — never block painting on revalidation.

How to test: every qxa case that add-samples; the user-project renders
(loading real WAVs, including truncated ones).

Known debt: swaveformdraw is shared by other renderers (cut) — candidate
for a render-support module when slices become real targets.
