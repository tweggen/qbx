# Proposal 22 — Clip pitch (cents), +/- semitone nudging

Status: EXECUTED (2026-07-21)

## Motivation

Clips could be time-stretched (Ctrl-drag on a clip edge → `resize-clip` with an
exact `stretch` fraction) but not transposed. A clip needs a pitch offset that
is independent of its length: select it, press `+` / `-`, transpose by a
semitone. Storage unit is CENTS (1/100 semitone) so finer edits and future
automation have somewhere to live.

## Approach: reuse the grain stage

No new DSP. `twGrainSource` already decouples the two axes:

- time-stretch changes the grain SPACING (output hop `Ho` vs input hop
  `Ho/stretch`), so the duration changes and the pitch does not;
- `pitchCents` changes the read rate INSIDE each grain
  (`r = 2^(cents/1200)`, `sp = inPos + j*r`), so the pitch changes and the
  duration does not.

Both were already implemented and `twGrainParams::pitchCents` was already
serialized by `SCut` — the pitch axis was simply not reachable from any
undoable, scriptable command. Because a pitch-only edit leaves the grain output
length equal to the input length, NO position math is touched: `srcStart_`,
`cutDuration_`, `clipToSourceMap`, the twTimeMaps and every clip edge stay put.

## What was implemented

1. **`set-pitch` action** (`main/objects/cut/ssetpitchaction.{h,cpp}`), attrs
   `clip`, `cents`, `take`, `broadcast`. The value is ABSOLUTE (not a delta) so
   undo restores an exact prior value even when the edit clamped, and scripts
   can state a pitch outright. On a take stack it targets ONE take (pitch is a
   per-take parameter, invariant 1) and bakes the resolved take index into the
   inverse. Edit-group broadcast mirrors `resize-clip`.
2. **Keyboard gesture** in the arranger (`SStdMixerView`): `+`/`-` = ±100 cents,
   `Shift`+`+`/`-` = ±10 cents, with `=` and numpad alternates. Targets are the
   current selection, falling back to the last-clicked clip; each clip gets its
   own absolute target (current + step) inside ONE composite undo step, so
   intervals between differently-pitched clips survive a nudge. Also in the clip
   context menu, and `SMainWindow::runSetClipPitch()` (the exact-value dialog)
   now routes through the same action instead of mutating the cut directly.
3. **Model fix**: `SCut::setGrainParams()` now `invalidateCapture()`s like
   `setWindow()` does. A grained cut's capture bakes the grain params in and
   `buildCapture_()` early-returns while a capture exists, so without it the
   waveform PREVIEW kept drawing the previous transform forever. (Playback was
   never affected — it grains the raw source.) Pitch is clamped centrally by
   `SCut::clampPitchCents` / `PITCH_CENTS_LIMIT` (±2400).
4. **Clip badge**: a transposed clip draws `+2 st` / `+250 ct` top-left, since a
   transposition is otherwise invisible in the arranger (it moves no edge).
5. **`assert-audio-frequency`** + `audio::estimateFundamental()` (autocorrelation
   with a first-strong-peak rule, parabolic peak interpolation). RMS and peak
   cannot distinguish a transposed render from an untransposed one; this can, and
   it is what makes the pitch cases real gates. The first-peak rule is load-
   bearing: plain max-of-autocorrelation reported the +1200 cent render at its
   ORIGINAL 440 Hz (the classic octave error — the 2× lag peaked higher).

## Tests

`tests/cases/grain_pitch_octave_up.qxa` (f0 doubles, length unchanged),
`grain_pitch_semitone_down.qxa` (r < 1 side), `grain_pitch_with_stretch.qxa`
(2× stretch AND +1200 cents — both axes at once, neither leaking),
`grain_pitch_reset_roundtrip.qxa` (pitch back to 0 returns to ~440 Hz, gating
the reader-chain rebuild).

## Known limits / follow-ups

- Fixed 2048/512 grain: overlapping grains splice content `Ho·(1−r)` frames
  apart, so large shifts comb, and an up-shift loses ~one grain of tail
  (grains read past the source end). Formant correction / phase-aligned or
  pitch-adaptive grains would be a separate proposal.
- `twGrainSource` materialises the WHOLE source in its constructor on the UI
  thread, so every keypress re-renders the clip's source. Same cost the stretch
  gesture already pays; if held-key repeat becomes a problem, coalesce through
  the existing `queueWindowParamEvent` mechanism rather than splitting the undo
  step.
- Keyboard layouts: on a US layout `+` IS `Shift`+`=`, so the Shift (fine) row is
  ambiguous there; the numpad bindings are unambiguous on every layout.
