# Proposal 26 — Rubber Band as the grain time-stretch / pitch-shift backend

> **Status: EXECUTED (2026-07-24).** Prerequisite reading: `06` (grain
> source), `18_EXACT_POSITION_DOMAINS.md` (WarpedPos/WarpedLen, exact length
> at the render boundary), `22_CLIP_PITCH.md` (the pitch cents → grain stage).
> Related but independent: `25_RENDER_TRANSFORM_PUSHDOWN.md` (compose the
> transform to the leaf) — 26 upgrades the *synthesis quality* of the stage 25
> also relies on, so it lands cleanly underneath 25.

## Motivation

`twGrainSource` is a naive fixed-hop time-slice overlap-add (OLA). On steady
tonal material it produces audible **amplitude modulation**: two overlapping
grains read from input positions spaced by `Hi = Ho/stretch`, which is not a
multiple of the signal period, so in each crossfade the grains are out of phase
and partially cancel — a periodic level dip at the hop rate. Reported in use: a
dull single-note hummed voice, stretched, warbles badly.

The existing `wsum` normalization divides by accumulated *window weight*, not
signal energy, so it corrects gain but **cannot** recover phase cancellation.
Windowing tricks therefore do not help; the fix has to align grain phase
(WSOLA/PSOLA) or move to the frequency domain (phase vocoder).

Rather than hand-roll WSOLA, adopt **Rubber Band Library v4.0** (Breakfast
Quay) — the open-source quality reference (Ardour, Mixxx, LMMS), a phase-vocoder
engine with identity phase-locking. Decided with the requester:

- Quality over license-permissiveness: **Rubber Band**, accepting its
  **GPL v2+** terms. **Smaragd ships GPL.** (Alternatives weighed and rejected:
  SoundTouch/LGPL — WSOLA, lower quality; Signalsmith/MIT — good but we chose
  Rubber Band's fidelity.)

## Key insight — the swap is internal and length-preserving

`twGrainSource` already materializes the whole warped signal ONCE into a
resident planar Float32 buffer in its constructor, after which `read()` is a
lock-free `memcpy`. Rubber Band's **offline mode** is exactly this shape: a
two-pass, whole-signal-in / whole-signal-out API. So we replace only the
constructor's fill loop — **no call site, no header, no domain math changes**.

The two construction sites (`main/objects/cut/src/scut.cpp:156,366`) and the
public header (`twgrainsource.h`) are untouched. Rubber Band is a **private**
include in `twgrainsource.cc`; it never leaks into a public header.

The one load-bearing invariant: proposal 18 makes
`nFrames_ = floor(inLen × stretchFrac)` (exact rational) the render-boundary
length, and the cut window lives in this WarpedLen domain. Rubber Band's output
count is only *approximately* `timeRatio × inLen`. **We clamp/zero-pad Rubber
Band's drained output to exactly `nFrames_`** before it lands in `data_`, so
`length()` is byte-for-byte the same contract as today and all position math is
untouched.

## Parameter mapping (1:1)

| twGrainParams | Rubber Band |
|---|---|
| `stretch` (Fraction, out/in) | `initialTimeRatio = stretch.approxDouble()` |
| `pitchCents` | `initialPitchScale = pow(2, pitchCents/1200)` (the existing `r`) |
| `grainSize`, `crossfade` | **vestigial** — RB manages its own windowing; kept in the struct for serialization compat, ignored on the RB path |

`isIdentity()` is unchanged, so untouched clips still skip the stage entirely.

## Offline sequence (v4.0)

```cpp
using RB = RubberBand::RubberBandStretcher;
RB rb(rate, channels,
      RB::OptionProcessOffline | RB::OptionEngineFiner | RB::OptionThreadingNever,
      timeRatio, pitchScale);
rb.setDebugLevel(0);                        // no direct-stderr chatter (logging policy)
rb.setExpectedInputDuration(inLen);
rb.setMaxProcessSize(kBlock);               // pre-size internal buffers to the block
rb.study(inPtrs, inLen, true);              // pass 1: analyse the whole signal
for (pos = 0; pos < inLen; pos += kBlock) { // pass 2: feed BOUNDED blocks...
    rb.process(inPtrs+pos, min(kBlock,inLen-pos), /*final=*/pos+block>=inLen);
    drain();                                // ...draining ready output after each
}
drain();                                    // flush the remainder
// then copy/zero-pad the drained output to exactly nFrames_ into data_.
```

- **Block-fed, not one-shot.** A single whole-clip `process(inLen)` overflows
  Rubber Band's output ring for any stretch whose output exceeds it (~262144
  frames): it drops samples (`RingBuffer::write: … only room for …`) and then
  grows the buffer, spamming stderr AND corrupting the warp — the cause of a
  user-reported *missing stretched signal* (direct and asset-captured) on load.
  Feeding bounded `kBlock`=4096 blocks and draining after each keeps the ring
  bounded; the offline result is unchanged, it just drives the library correctly.
  `setMaxProcessSize`/`setDebugLevel(0)` complete the fix (pre-sized buffers, no
  library stderr — the project routes all logging through `TW_LOG`).
- `OptionEngineFiner` = the R3 engine (best quality). `OptionThreadingNever`
  keeps the offline pass single-threaded → **deterministic**, so the flake gate
  (`repeat_test.sh`) stays N/N and the non-grain byte-exact `cmp` gate is
  unaffected (grain never feeds those cases). Verified: 5/5 identical RMS to the
  last digit; loop case 8/8 at workers ∈ {1,8}.
- **Formants are left at the default** (they scale with pitch — no preservation).
  Preservation is a source-filter/vocal assumption that colours and *de-energises*
  general material (measured: an octave-up on the sawtooth fixture dropped to
  ~0.52× RMS with preservation on, ~1.0× with it off). A per-clip formant toggle
  can wire it back for voice as a later, opt-in feature.
- Input is read the way the current ctor already does it — planar, per channel
  via `src.read(0, in.data(), inLen, c)` — then presented as `float* const*`.
  **Bonus:** feeding all channels together keeps stereo phase-coherent (today's
  per-channel independent loop does not).

## Loudness preservation — NO output gain processing

The engine applies **no gain to the warp**. Rubber Band's R3 resynthesis is
loudness-preserving, so a time-stretch or pitch-shift keeps the source RMS —
measured on the sawtooth fixture: 2× stretch → 0.266, ½× → 0.262, octave-up →
0.264, all against the source's ~0.267. A longer clip carries proportionally
more *total* energy; the *level* is what stays constant — the mathematically
expected behaviour (requester's framing).

An earlier version added a global peak-scaling "anti-clip" (attenuate the whole
warp when its peak breached a ceiling). It was **removed**: a single Gibbs
transient of ~1.2 pulled the whole clip down ~1.7 dB (0.266 → 0.219 on 2×),
breaking exactly that loudness invariance. The right trade is to leave the
signal alone: Rubber Band's phase-coherent reconstruction legitimately produces
a *higher instantaneous peak* than the input at the same RMS (Gibbs at sharp
edges — the 0.798-peak sawtooth reaches full-scale). On the render, any rare
overshoot on near-full-scale material is clamped by the format conversion at the
boundary, exactly as for any other signal; material with headroom (a recording,
the hummed voice) never gets near it. A per-clip make-up gain, if ever wanted,
is an opt-in feature, not a hidden default.

## Build wiring

- `tw303a/CMakeLists.txt`: Rubber Band ships a pkg-config file but **no CMake
  config package**, so discovery is `find_path`/`find_library` on Windows (the
  vcpkg tree is on `CMAKE_PREFIX_PATH` via the toolchain) and `pkg_check_modules`
  on macOS/Linux. If found, link it PRIVATE into `tw_sources` and define
  `TW_HAVE_RUBBERBAND=1`; otherwise a build WARNING and the legacy fallback.
- **vcpkg overlay port** (`smaragd/vcpkg-overlays/rubberband/`): the upstream
  port forces the `sleef` FFT on x64-windows, and sleef fails to build under
  MinGW. The overlay switches x64-windows to Rubber Band's dependency-free
  **builtin** FFT (adequate — the warp is offline). `--overlay-ports` is wired
  into `ensure_render_deps`. macOS/Linux keep upstream (fftw/vDSP).
- `twgrainsource.cc`: `#if TW_HAVE_RUBBERBAND` → Rubber Band path; `#else` →
  the legacy OLA (kept verbatim as the fallback/reference). A graceful degrade
  means a machine without the dep still builds and makes sound — it just does
  not get the quality upgrade, and logs it once.
- `_env.sh` `ensure_render_deps`: add `rubberband` to the Windows vcpkg install
  list (keyed off `share/rubberband/` presence), so `build.sh`/`rebuild.sh`
  bootstrap it automatically alongside libsndfile/libvorbis.
- macOS: `brew install rubberband` (Accelerate/vDSP FFT path, arm64-native);
  vcpkg `arm64-osx` also works if uniformity is preferred.

## Re-rendering & testing

The grain qxa cases assert on **physically-grounded tolerances**, not golden
bytes: `assert-audio-energy` (RMS band), `assert-audio-peak`,
`assert-audio-frequency` (±3%). The only committed WAV is the *input* fixture
`test_sawtooth.wav`. So re-baselining = rebuild, re-run, and widen a tolerance
band only where Rubber Band legitimately shifts it.

Cases exercised: `grain_time_stretch_2x`, `grain_time_stretch_half`,
`grain_minimal_stretch`, `grain_with_energy_verification`,
`grain_multiple_stretch_factors`, `grain_with_volume_control`,
`grain_loop_stretch`, `grain_pitch_octave_up`, `grain_pitch_semitone_down`,
`grain_pitch_with_stretch`, `grain_pitch_reset_roundtrip`,
`grain_split_delete_crash`, `exact_stretch_roundtrip`, plus the
`sources_test` unit (grain length within tolerance / ratio ≈ stretch).

Outcome (measured, deterministic):
- **Length / silence-after-clip**: green untouched everywhere — the exact
  `nFrames_` clamp holds. Silence bands at the exact clip end read 0.0.
- **Frequency (±3%)**: green and *more* accurate than the old path (octave-up
  879.76 Hz, fifth-down 293.78 Hz — essentially exact).
- **Peak (`≤ 0.995`)**: green — the true-peak safety caps overshoot at 0.99 so
  the existing bounds hold unchanged.
- **RMS bands are UNCHANGED from the old overlap-add.** Because Rubber Band is
  loudness-preserving (once the anti-clip was removed and the driving corrected),
  every original OLA-calibrated RMS band still holds — the earlier round of band
  recalibration was compensating for the anti-clip artifact and was reverted.
  Identity-path renders (stretch=1, no pitch) skip grain and stay byte-identical.
- **Peak bands**: the only test change. Nine sawtooth cases now reach full-scale
  (Gibbs overshoot, RMS preserved), so their `maxPeak` was relaxed 0.995 → 1.0
  with a comment. Real material with headroom stays below.
- **New regression** `grain_asset_stretch.qxa`: a x2-stretched clip on track 0
  (384000-frame output — the size that overflowed the one-shot `process()`)
  captured by an asset on track 1; asserts the stretched signal is present on
  BOTH the direct and asset-captured paths (RMS 0.266 each). Guards the
  user-reported missing-signal.

Result: **14/14 grain + stretch cases green (incl. the new asset case), 11/11
asset/container/edit cases, 12/12 module tests, sources_test green, loop case
8/8 deterministic at workers {1,8}, non-grain byte-exact cases unaffected.**
Determinism gate: `repeat_test.sh` over the grain cases (RB offline is
deterministic with `OptionThreadingNever`).

## Risks / follow-ups

- **GPL** is now load-bearing for the whole app — noted in CLAUDE.md/deps.
- **Offline build-once cost**: RB R3 is heavier than OLA, but this is the
  offline warp materialized once per (source, params) and cached in `data_`;
  `read()` stays a `memcpy`. Acceptable. If a very long clip ever makes ctor
  latency visible, block-feeding + a progress hook is the escape hatch.
- **Live/variable rate** (automation) still needs a streaming node — out of
  scope here, same as proposal 06 noted. RB has a realtime mode + a
  `RubberBandLiveShifter` for that future work.
- Interaction with **proposal 25** (transform pushdown): 26 only improves the
  leaf grain stage 25 pushes down to; they compose, 26 lands first.
