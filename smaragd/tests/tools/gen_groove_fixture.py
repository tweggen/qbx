#!/usr/bin/env python3
"""Generator for smaragd/tests/groove/*.wav + manifest.json -- the M0 fixture
set of proposal 40 (groove/feel resonance model), section 6 milestone M0.

NOT A TEST. It lives outside tests/cases/ so the CONFIGURE_DEPENDS glob never
registers it, and it is committed for the same reason gen_auto_fixture.py and
gen_clip_fixture.py are: a fixture nobody can regenerate is a fixture nobody
can reason about.

    cd smaragd/tests/tools
    python gen_groove_fixture.py            # writes tests/groove/*.wav + manifest.json
    python gen_groove_fixture.py --verify    # re-reads them, checks construction

Everything here is DETERMINISTIC: every random draw goes through a
`random.Random(seed)` instance seeded from the fixed SEEDS table below, so two
runs on two machines produce byte-identical WAVs. Stdlib only (wave / struct /
math / random / json / argparse, plus `array` and `collections` for speed --
both stdlib).

SOUND VOCABULARY (all synthesized, never sampled)
--------------------------------------------------
  LOW click       80 Hz sine, 0.5 ms linear attack, ~30 ms exponential decay.
  HIGH click      4000 Hz sine, 0.5 ms attack, ~10 ms exponential decay.
  BROADBAND click 5 ms seeded white noise, 0.5 ms attack AND release ramps
                  (the release is a deliberate addition beyond the letter of
                  the spec -- see the deviations note at the bottom of this
                  docstring -- so the burst does not end on a step edge).
  SWEPT kick      sine sweeping 120 -> 50 Hz over 60 ms (phase integrated,
                  not `sin(2*pi*f(t)*t)`, so the sweep is phase-continuous),
                  then holding at 50 Hz, under the same decay envelope as
                  the static kick.
  STATIC kick     60 Hz sine, same attack/decay envelope as the swept kick.
  X / Y streams   300 Hz / 350 Hz sine, 0.5 ms attack, 30 ms decay -- the
                  "two overlapping-band instruments" fixture (f).
  WASH            continuous seeded white noise, crudely lowpassed by an
                  8-sample moving average, held at -12 dB relative to a
                  click's peak (1.0 pre-mix).

Every "exponential decay ~X ms" envelope is `exp(-(n-attack)/tau)` with
`tau` in samples equal to X ms, and its burst is truncated once the envelope
has fallen 60 dB (tau * ln(1000) =~ 6.908 * tau samples past the attack) --
long enough to be inaudible, short enough to keep the fixtures small.

GAIN / HEADROOM
----------------
Every voice is synthesized at pre-mix peak 1.0 (WASH at 10**(-12/20) =
0.2512, i.e. -12 dB relative to a click). Each fixture's mixed buffer is then
scaled by ONE scalar gain so its true peak sits at TARGET_PEAK = 0.9 before
16-bit quantization -- this can never clip, and it never changes the RELATIVE
level between voices (a linear scale of the whole mix preserves every ratio,
in particular WASH's -12 dB). The applied gain is recorded per-fixture in the
manifest as "gainApplied".

MANIFEST
--------
Top level: {"rate": 48000, "fixtures": {name: {...}}}. Each fixture entry
carries a "_doc" one-liner, "file", "durationNominalSec", "durationFrames"
(the actual, padded, file length), "padFrames", "gainApplied", "voiceLevels"
(pre-gain nominal peak per voice used), an "events" dict of
{streamName: [frame, ...]} -- the exact construction, ALWAYS the integer
sample frame actually used to place the burst (irrational-BPM event times are
computed in continuous time and rounded once, at placement, never twice) --
plus fixture-specific ground-truth fields named in the task list below, and a
"verify" list of stream descriptors ({name, band, tolMs, minSepMs, regions})
that --verify uses to re-derive and check "events" independently from the
audio, by simple energy-based peak-picking.

--verify
--------
For every fixture: read the WAV back, split it into a "low" (via a 48-tap
moving-average lowpass -- 48000/48 = 1000 Hz spacing between nulls, so
4000 Hz falls exactly on the 4th null while an 80 Hz click passes through
almost unattenuated) and "high" (= original - low) band, run a simple blind
peak-picker
(short-time energy against an adaptive local baseline, greedy non-max
suppression, refined to the sample-accurate local energy peak) over each
declared stream, match detections to the constructed "events" by nearest
neighbour, and check: no missing event, no spurious extra event, and the
worst residual against tolMs. A handful of fixtures (b_jitter's per-segment
sigma, h_fill_break's silence window) get one extra fixture-specific check
below the generic pass. Exit code is nonzero iff anything failed; a
PASS/FAIL table is printed either way.

This is a CONSTRUCTION check on the generator, not a scientific validator --
tolerances are generous (a few ms) because the block-energy detector's own
resolution is on that order; it exists to catch "the loop bound is off by
one", "the offset has the wrong sign", "the gain silently canceled a voice",
never to reproduce proposal 40 section 6's own (much tighter) M0 ACs, which
belong to `groove_test` once the estimator exists.

DEVIATIONS FROM THE LETTER OF THE SPEC (and why)
-------------------------------------------------
  * BROADBAND clicks get a symmetric 0.5 ms RELEASE ramp in addition to the
    spec'd attack ramp, so a burst never ends on a hard sample-domain step
    (which would itself read as a broadband click layered on the intended
    one). Content and duration are otherwise exactly as specified.
  * Kick and X/Y decay TIMES are not given numeric values by the spec beyond
    X's "30 ms decay" (reused for Y) -- kicks use tau=40 ms, chosen to read
    as a low "thud" comparable to the LOW click's 30 ms and give the 60 ms
    sweep room to complete before the amplitude envelope has collapsed.
  * Every voice gets the same 0.5 ms attack ramp the spec states for clicks,
    including kicks and X/Y, for consistency and to avoid a startup step.
  * Every "N s" fixture duration is the nominal EVENT-GRID span; the actual
    file is padded by (longest voice tail used) + 10 ms so the last event's
    decay is never truncated. "durationFrames" in the manifest is the actual
    (padded) length; "durationNominalSec" is the spec'd span.
  * d_twobar's "every second bar" is read as bar indices 0, 2, 4, ... (the
    first bar of every pair), 0-indexed; h_fill_break's 8/1/2/8-bar section
    plan is read as contiguous with no additional gap bars; f's "even/odd
    8ths" are read as 8th-grid indices n=0,2,4,... (X) and n=1,3,5,... (Y)
    over one continuous 8th-note counter spanning the whole file, not two
    independent counters. All of these are recorded explicitly in each
    fixture's manifest entry so nothing is silently assumed downstream.
"""

import argparse
import json
import math
import os
import random
import sys
import wave
from array import array
from collections import namedtuple

SR = 48000
HERE = os.path.dirname(os.path.abspath(__file__))
OUTDIR = os.path.join(HERE, "..", "groove")
MANIFEST_PATH = os.path.join(OUTDIR, "manifest.json")

TARGET_PEAK = 0.9
ATTACK_MS = 0.5
ATTACK_SAMPLES = int(round(ATTACK_MS * SR / 1000.0))   # 24
TAIL_K = 6.908                                          # -60 dB point: tau*ln(1000)

LOW_FREQ = 80.0
LOW_TAU_MS = 30.0
HIGH_FREQ = 4000.0
HIGH_TAU_MS = 10.0
KICK_TAU_MS = 40.0
KICK_SWEEP_MS = 60.0
KICK_SWEEP_F0 = 120.0
KICK_SWEEP_F1 = 50.0
STATIC_KICK_FREQ = 60.0
XY_TAU_MS = 30.0
X_FREQ = 300.0
Y_FREQ = 350.0
BROADBAND_MS = 5.0
BROADBAND_RAMP_MS = 0.5
WASH_DB = -12.0
WASH_LEVEL = 10.0 ** (WASH_DB / 20.0)
WASH_MA_WINDOW = 8

SEEDS = {
    "a0_broadband_grid": 4001,
    "a_offset15": 4002,
    "b_jitter": 4003,
    "c_tempo_drift": 4004,
    "d_twobar": 4005,
    "e_sweep_vs_static": 4006,
    "e_static_only": 4007,
    "e_sweep_only": 4008,
    "f_overlap_streams": 4009,
    "g_wash": 4010,
    "h_fill_break": 4011,
    "i_tempo_pair": 4012,
}

# ---------------------------------------------------------------------------
# voice synthesis
# ---------------------------------------------------------------------------


def ms_to_samples(ms):
    return int(round(ms * SR / 1000.0))


def _envelope(length, attack_samples, tau_samples):
    env = array('d', [0.0]) * length
    for n in range(length):
        if attack_samples > 0 and n < attack_samples:
            env[n] = n / float(attack_samples)
        else:
            t = n - attack_samples
            env[n] = math.exp(-t / tau_samples)
    return env


def sine_burst(freq, tau_ms, attack_samples=ATTACK_SAMPLES):
    tau_samples = tau_ms * SR / 1000.0
    length = attack_samples + int(math.ceil(TAIL_K * tau_samples))
    env = _envelope(length, attack_samples, tau_samples)
    w = 2.0 * math.pi * freq / SR
    buf = array('d', [0.0]) * length
    for n in range(length):
        buf[n] = env[n] * math.sin(w * n)
    return buf


def swept_sine_burst(f0, f1, sweep_ms, tau_ms, attack_samples=ATTACK_SAMPLES):
    tau_samples = tau_ms * SR / 1000.0
    sweep_samples = ms_to_samples(sweep_ms)
    length = attack_samples + int(math.ceil(TAIL_K * tau_samples))
    length = max(length, sweep_samples + 1)
    env = _envelope(length, attack_samples, tau_samples)
    buf = array('d', [0.0]) * length
    phase = 0.0
    for n in range(length):
        if n < sweep_samples:
            frac = n / float(sweep_samples)
            freq = f0 + (f1 - f0) * frac
        else:
            freq = f1
        phase += 2.0 * math.pi * freq / SR
        buf[n] = env[n] * math.sin(phase)
    return buf


def broadband_burst(rng, total_ms=BROADBAND_MS, ramp_ms=BROADBAND_RAMP_MS):
    total = ms_to_samples(total_ms)
    ramp = ms_to_samples(ramp_ms)
    buf = array('d', [0.0]) * total
    for n in range(total):
        if n < ramp:
            g = n / float(ramp)
        elif n >= total - ramp:
            g = (total - 1 - n) / float(ramp)
        else:
            g = 1.0
        buf[n] = g * rng.uniform(-1.0, 1.0)
    return buf


def wash_noise(n, rng, ma_window=WASH_MA_WINDOW, level=WASH_LEVEL):
    raw = array('d', [0.0]) * n
    for i in range(n):
        raw[i] = rng.uniform(-1.0, 1.0)
    prefix = [0.0] * (n + 1)
    for i in range(n):
        prefix[i + 1] = prefix[i] + raw[i]
    half = ma_window // 2
    buf = array('d', [0.0]) * n
    for i in range(n):
        lo = max(0, i - half)
        hi = min(n, i + (ma_window - half))
        buf[i] = (prefix[hi] - prefix[lo]) / (hi - lo)
    peak = max((abs(v) for v in buf), default=0.0)
    scale = (level / peak) if peak > 1e-12 else 1.0
    for i in range(n):
        buf[i] *= scale
    return buf


# pre-built, purely deterministic voices (no per-fixture RNG needed) -------
LOW_VOICE = sine_burst(LOW_FREQ, LOW_TAU_MS)
HIGH_VOICE = sine_burst(HIGH_FREQ, HIGH_TAU_MS)
STATIC_KICK_VOICE = sine_burst(STATIC_KICK_FREQ, KICK_TAU_MS)
SWEPT_KICK_VOICE = swept_sine_burst(KICK_SWEEP_F0, KICK_SWEEP_F1, KICK_SWEEP_MS, KICK_TAU_MS)
X_VOICE = sine_burst(X_FREQ, XY_TAU_MS)
Y_VOICE = sine_burst(Y_FREQ, XY_TAU_MS)

VOICE_LEN = {
    "low": len(LOW_VOICE),
    "high": len(HIGH_VOICE),
    "static_kick": len(STATIC_KICK_VOICE),
    "swept_kick": len(SWEPT_KICK_VOICE),
    "x": len(X_VOICE),
    "y": len(Y_VOICE),
    "broadband": ms_to_samples(BROADBAND_MS),
}


def place(mixbuf, voice, start_frame, gain=1.0):
    n = len(mixbuf)
    s = int(round(start_frame))
    lo = max(0, -s)
    hi = min(len(voice), n - s)
    for i in range(lo, hi):
        mixbuf[s + i] += voice[i] * gain


# ---------------------------------------------------------------------------
# grid / tempo helpers
# ---------------------------------------------------------------------------


def grid_frames(interval_frames, duration_sec):
    """Frames n*interval (n=0,1,2,...) with n*interval < duration_sec*SR,
    rounded to the nearest integer sample at placement time (never twice)."""
    limit = duration_sec * SR
    out = []
    n = 0
    while True:
        f = n * interval_frames
        if f >= limit - 1e-9:
            break
        out.append(int(round(f)))
        n += 1
    return out


def tempo_ramp_event_frames(bpm0, bpm1, ramp_sec, subdiv_per_beat=2):
    """Event k occurs at time t_k solving phase(t_k) = k, where
    phase(t) = integral_0^t bpm(u)/60 * subdiv_per_beat du and
    bpm(u) = bpm0 + (bpm1-bpm0)*u/ramp_sec is linear in u.

    phase(t) = (subdiv_per_beat/60) * (bpm0*t + (bpm1-bpm0)*t^2/(2*ramp_sec))

    Solving the quadratic A*t^2 + B*t - k = 0 for the positive root, with
    A = (bpm1-bpm0)*subdiv_per_beat/(120*ramp_sec), B = bpm0*subdiv_per_beat/60.
    """
    A = (bpm1 - bpm0) * subdiv_per_beat / (120.0 * ramp_sec)
    B = bpm0 * subdiv_per_beat / 60.0
    frames = []
    k = 0
    while True:
        if abs(A) < 1e-12:
            t = k / B
        else:
            disc = B * B + 4.0 * A * k
            if disc < 0:
                break
            t = (-B + math.sqrt(disc)) / (2.0 * A)
        if t >= ramp_sec - 1e-9:
            break
        frames.append(int(round(t * SR)))
        k += 1
    return frames


def pad_frames_for(voice_names, margin_ms=10.0):
    longest = max(VOICE_LEN[v] for v in voice_names)
    return longest + ms_to_samples(margin_ms)


# ---------------------------------------------------------------------------
# WAV IO
# ---------------------------------------------------------------------------


def write_wav(path, floatbuf, target_peak=TARGET_PEAK):
    peak = max((abs(v) for v in floatbuf), default=0.0)
    gain = (target_peak / peak) if peak > 1e-9 else 1.0
    n = len(floatbuf)
    ints = array('h', [0]) * n
    for i in range(n):
        s = floatbuf[i] * gain
        if s > 1.0:
            s = 1.0
        elif s < -1.0:
            s = -1.0
        ints[i] = int(round(s * 32767.0))
    with wave.open(path, 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(ints.tobytes())
    return gain


def read_wav_mono_float(path):
    with wave.open(path, 'rb') as w:
        assert w.getnchannels() == 1, path
        assert w.getsampwidth() == 2, path
        n = w.getnframes()
        raw = w.readframes(n)
    ints = array('h')
    ints.frombytes(raw)
    return array('d', (v / 32768.0 for v in ints))


# ===========================================================================
# fixture builders
# ===========================================================================

FixtureResult = namedtuple("FixtureResult", "mixbuf manifest")


def _new_entry(doc, file_name, duration_nominal_sec, pad, bpm=None):
    e = {
        "_doc": doc,
        "file": file_name,
        "durationNominalSec": duration_nominal_sec,
        "padFrames": pad,
        "voiceLevels": {},
        "events": {},
        "verify": [],
    }
    if bpm is not None:
        e["bpm"] = bpm
    return e


def build_a0_broadband_grid():
    bpm = 120.0
    dur = 16.0
    interval = SR * 30.0 / bpm  # eighth interval, exact at 120 bpm = 12000
    pad = pad_frames_for(["low", "high", "broadband"])
    n = int(round(dur * SR)) + pad
    mix = array('d', [0.0]) * n

    grid = grid_frames(interval, dur)
    rng = random.Random(SEEDS["a0_broadband_grid"])
    for f in grid:
        place(mix, LOW_VOICE, f)
        place(mix, HIGH_VOICE, f)
        place(mix, broadband_burst(rng), f)

    e = _new_entry(
        "8th-grid at 120 BPM; LOW+HIGH+BROADBAND exactly simultaneous on every "
        "grid point. Ground truth: all inter-voice offsets are 0.",
        "a0_broadband_grid.wav", dur, pad, bpm=bpm)
    e["voiceLevels"] = {"low": 1.0, "high": 1.0, "broadband": 1.0}
    e["events"] = {"low": grid, "high": grid, "broadband": grid}
    e["groundTruth"] = {"deltaMuHighMinusLowMs": 0.0, "deltaMuBroadbandMinusLowMs": 0.0}
    e["verify"] = [
        {"name": "low", "band": "low", "tolMs": 5.0, "minSepMs": 100.0, "regions": [[0, n]]},
        {"name": "high", "band": "high", "tolMs": 5.0, "minSepMs": 100.0, "regions": [[0, n]]},
    ]
    return FixtureResult(mix, e)


def build_a_offset15():
    bpm = 120.0
    dur = 16.0
    interval = SR * 30.0 / bpm
    offset_ms = 15.0
    offset_fr = ms_to_samples(offset_ms)
    pad = pad_frames_for(["low", "high"])
    n = int(round(dur * SR)) + pad
    mix = array('d', [0.0]) * n

    grid = grid_frames(interval, dur)
    high_frames = [f + offset_fr for f in grid]
    for f in grid:
        place(mix, LOW_VOICE, f)
    for f in high_frames:
        place(mix, HIGH_VOICE, f)

    e = _new_entry(
        "8th-grid at 120 BPM; LOW exactly on grid, HIGH +15.0 ms late. "
        "Ground truth: deltaMuHighMinusLowMs = +15.0.",
        "a_offset15.wav", dur, pad, bpm=bpm)
    e["voiceLevels"] = {"low": 1.0, "high": 1.0}
    e["events"] = {"low": grid, "high": high_frames}
    e["groundTruth"] = {"deltaMuHighMinusLowMs": offset_ms}
    e["verify"] = [
        {"name": "low", "band": "low", "tolMs": 4.0, "minSepMs": 100.0, "regions": [[0, n]]},
        {"name": "high", "band": "high", "tolMs": 3.0, "minSepMs": 100.0, "regions": [[0, n]]},
    ]
    return FixtureResult(mix, e)


def _gaussian_clamped(rng, sigma_ms, clamp_sigma=3.0):
    if sigma_ms <= 0.0:
        return 0.0
    v = rng.gauss(0.0, sigma_ms)
    lim = clamp_sigma * sigma_ms
    if v > lim:
        v = lim
    elif v < -lim:
        v = -lim
    return v


def build_b_jitter():
    bpm = 120.0
    interval = SR * 30.0 / bpm  # 12000, exact
    seg_sec = 8.0
    gaussian_sigmas = [0.0, 3.0, 6.0, 12.0, 24.0]
    ar1_specs = [(12.0, 0.5), (12.0, 2.0), (12.0, 8.0)]
    n_segs = len(gaussian_sigmas) + len(ar1_specs)
    total_dur = n_segs * seg_sec
    pad = pad_frames_for(["low", "high"])
    n = int(round(total_dur * SR)) + pad
    mix = array('d', [0.0]) * n

    rng = random.Random(SEEDS["b_jitter"])

    low_frames = grid_frames(interval, total_dur)
    for f in low_frames:
        place(mix, LOW_VOICE, f)

    high_frames = []
    high_nominal = []
    segments_gt = []
    dt_ms = interval * 1000.0 / SR  # 250 ms

    for seg_idx in range(n_segs):
        seg_start_sec = seg_idx * seg_sec
        seg_end_sec = seg_start_sec + seg_sec
        seg_start_frame = int(round(seg_start_sec * SR))
        seg_end_frame = int(round(seg_end_sec * SR))
        nominal_in_seg = [f for f in low_frames if seg_start_frame <= f < seg_end_frame]

        if seg_idx < len(gaussian_sigmas):
            sigma = gaussian_sigmas[seg_idx]
            kind = "gaussian"
            tau = None
            deltas_ms = [_gaussian_clamped(rng, sigma) for _ in nominal_in_seg]
        else:
            sigma, tau_sec = ar1_specs[seg_idx - len(gaussian_sigmas)]
            kind = "ar1"
            tau = tau_sec
            a = math.exp(-dt_ms / 1000.0 / tau_sec)
            deltas_ms = []
            prev = _gaussian_clamped(rng, sigma)
            for i in range(len(nominal_in_seg)):
                if i == 0:
                    d = prev
                else:
                    innovation = rng.gauss(0.0, sigma)
                    d = a * prev + math.sqrt(max(0.0, 1.0 - a * a)) * innovation
                    lim = 3.0 * sigma
                    if d > lim:
                        d = lim
                    elif d < -lim:
                        d = -lim
                    prev = d
                deltas_ms.append(d)

        seg_high_frames = []
        for f_nom, d_ms in zip(nominal_in_seg, deltas_ms):
            fr = f_nom + d_ms * SR / 1000.0
            seg_high_frames.append(int(round(fr)))
        high_nominal.extend(nominal_in_seg)
        high_frames.extend(seg_high_frames)

        actual_ms = [(hf - fn) * 1000.0 / SR for hf, fn in zip(seg_high_frames, nominal_in_seg)]
        mean_ms = sum(actual_ms) / len(actual_ms) if actual_ms else 0.0
        var_ms = (sum((v - mean_ms) ** 2 for v in actual_ms) / len(actual_ms)) if actual_ms else 0.0
        std_ms = math.sqrt(var_ms)
        lag1 = None
        if kind == "ar1" and len(actual_ms) > 1:
            m = mean_ms
            num = sum((actual_ms[i] - m) * (actual_ms[i - 1] - m) for i in range(1, len(actual_ms)))
            den = sum((v - m) ** 2 for v in actual_ms)
            lag1 = (num / den) if den > 1e-12 else None

        segments_gt.append({
            "index": seg_idx,
            "kind": kind,
            "startFrame": seg_start_frame,
            "endFrame": seg_end_frame,
            "targetSigmaMs": sigma,
            "tauSec": tau,
            "measuredStdMs": std_ms,
            "measuredLag1": lag1,
            "n": len(actual_ms),
        })

    for f in high_frames:
        place(mix, HIGH_VOICE, f)

    e = _new_entry(
        "8th-grid at 120 BPM; LOW always on grid. HIGH jittered per 8 s segment: "
        "5 segments of Gaussian jitter (sigma 0/3/6/12/24 ms, clamped +-3 sigma), "
        "then 3 segments of AR(1) jitter (sigma=12 ms, tau=0.5/2/8 s, reset to a "
        "fresh stationary draw at each segment start).",
        "b_jitter.wav", total_dur, pad, bpm=bpm)
    e["voiceLevels"] = {"low": 1.0, "high": 1.0}
    e["events"] = {"low": low_frames, "high": high_frames, "highNominal": high_nominal}
    e["groundTruth"] = {"segments": segments_gt, "segmentDurationSec": seg_sec}
    e["verify"] = [
        {"name": "low", "band": "low", "tolMs": 5.0, "minSepMs": 100.0, "regions": [[0, n]]},
        # HIGH verify target is the actual constructed (jittered) frame, tight
        # tolerance -- this checks placement fidelity, not jitter recovery.
        {"name": "high", "band": "high", "tolMs": 4.0, "minSepMs": 60.0, "regions": [[0, n]],
         "matchAgainst": "high"},
    ]
    return FixtureResult(mix, e)


def build_c_tempo_drift():
    bpm0, bpm1 = 120.0, 122.0
    ramp_sec = 24.0
    pad = pad_frames_for(["low", "high"])
    n = int(round(ramp_sec * SR)) + pad
    mix = array('d', [0.0]) * n

    frames = tempo_ramp_event_frames(bpm0, bpm1, ramp_sec, subdiv_per_beat=2)
    for f in frames:
        place(mix, LOW_VOICE, f)
        place(mix, HIGH_VOICE, f)

    e = _new_entry(
        "8th-grid, LOW+HIGH simultaneous; tempo ramps linearly 120->122 BPM over "
        "24 s. Event k's time solves phase(t_k)=k for phase(t)=integral bpm(u)/30 "
        "du (eighths/sec = bpm/30); see tempo_ramp_event_frames()'s docstring "
        "for the closed-form quadratic. Ground truth: the exact frame list IS "
        "the tempo-integration ground truth.",
        "c_tempo_drift.wav", ramp_sec, pad)
    e["voiceLevels"] = {"low": 1.0, "high": 1.0}
    e["events"] = {"low": frames, "high": frames}
    e["groundTruth"] = {"bpmStart": bpm0, "bpmEnd": bpm1, "rampDurationSec": ramp_sec,
                         "formula": "phase(t)=(subdiv/60)*(bpm0*t+(bpm1-bpm0)*t^2/(2*ramp)); "
                                    "event k at phase(t_k)=k, subdiv=2 (eighths)"}
    e["verify"] = [
        {"name": "low", "band": "low", "tolMs": 3.0, "minSepMs": 80.0, "regions": [[0, n]]},
        {"name": "high", "band": "high", "tolMs": 3.0, "minSepMs": 80.0, "regions": [[0, n]]},
    ]
    return FixtureResult(mix, e)


def build_d_twobar():
    bpm = 120.0
    dur = 32.0
    quarter = SR * 60.0 / bpm  # 24000, exact
    bar_frames = 4 * quarter
    two_bar_frames = 2 * bar_frames
    pad = pad_frames_for(["low", "high"])
    n = int(round(dur * SR)) + pad
    mix = array('d', [0.0]) * n

    low_frames = grid_frames(quarter, dur)
    high_frames = []
    high_bar_indices = []
    for k, f in enumerate(low_frames):
        bar_idx = k // 4
        if bar_idx % 2 == 0 and k % 4 == 0:
            high_frames.append(f)
            high_bar_indices.append(bar_idx)

    for f in low_frames:
        place(mix, LOW_VOICE, f)
    for f in high_frames:
        place(mix, HIGH_VOICE, f)

    e = _new_entry(
        "120 BPM 4/4, 32 s; LOW on every quarter, HIGH only on beat 1 of every "
        "SECOND bar (0-indexed bars 0,2,4,... -- 'every second bar' read as "
        "the first bar of each pair). Ground truth: the 2-bar periodicity.",
        "d_twobar.wav", dur, pad, bpm=bpm)
    e["voiceLevels"] = {"low": 1.0, "high": 1.0}
    e["events"] = {"low": low_frames, "high": high_frames}
    e["groundTruth"] = {"quarterFrames": quarter, "barFrames": bar_frames,
                         "twoBarFrames": two_bar_frames, "highBarIndices": high_bar_indices}
    e["verify"] = [
        {"name": "low", "band": "low", "tolMs": 4.0, "minSepMs": 150.0, "regions": [[0, n]]},
        {"name": "high", "band": "high", "tolMs": 3.0, "minSepMs": 150.0, "regions": [[0, n]]},
    ]
    return FixtureResult(mix, e)


def build_e_sweep_vs_static():
    bpm = 100.0
    dur = 16.0
    quarter = SR * 60.0 / bpm  # 28800, exact
    bar_frames = 4 * quarter
    pad = pad_frames_for(["static_kick", "swept_kick"])
    n = int(round(dur * SR)) + pad
    mix = array('d', [0.0]) * n

    frames = grid_frames(quarter, dur)
    kicks = []
    for k, f in enumerate(frames):
        bar_idx = k // 4
        is_swept = (bar_idx % 2 == 0)
        voice = SWEPT_KICK_VOICE if is_swept else STATIC_KICK_VOICE
        place(mix, voice, f)
        kicks.append({"frame": f, "type": "swept" if is_swept else "static",
                       "quarterIndex": k, "barIndex": bar_idx})

    e = _new_entry(
        "100 BPM quarters, 16 s, alternating bars: even bars (0-indexed) get "
        "the SWEPT kick, odd bars the STATIC kick, at identical nominal grid "
        "times. See e_static_only.wav / e_sweep_only.wav for isolated "
        "single-type references on the SAME grid.",
        "e_sweep_vs_static.wav", dur, pad, bpm=bpm)
    e["voiceLevels"] = {"static_kick": 1.0, "swept_kick": 1.0}
    e["events"] = {"kicks": kicks}
    e["groundTruth"] = {"quarterFrames": quarter, "barFrames": bar_frames,
                         "pattern": "bar%2==0 (0-indexed) -> swept, else static"}
    all_frames = [k["frame"] for k in kicks]
    e["verify"] = [
        {"name": "kicks", "band": "low", "tolMs": 5.0, "minSepMs": 200.0, "regions": [[0, n]],
         "framesOverride": all_frames},
    ]
    return FixtureResult(mix, e)


def _build_e_single(name, voice, is_swept):
    bpm = 100.0
    dur = 8.0
    quarter = SR * 60.0 / bpm
    pad = pad_frames_for(["static_kick", "swept_kick"])
    n = int(round(dur * SR)) + pad
    mix = array('d', [0.0]) * n
    frames = grid_frames(quarter, dur)
    for f in frames:
        place(mix, voice, f)

    e = _new_entry(
        "100 BPM quarters, 8 s, single-type reference (%s only) on the SAME "
        "grid as e_sweep_vs_static.wav, for differential measurement." %
        ("swept" if is_swept else "static"),
        "%s.wav" % name, dur, pad, bpm=bpm)
    e["voiceLevels"] = {"swept_kick" if is_swept else "static_kick": 1.0}
    e["events"] = {"kicks": frames}
    e["groundTruth"] = {"quarterFrames": quarter, "type": "swept" if is_swept else "static"}
    e["verify"] = [
        {"name": "kicks", "band": "low", "tolMs": 5.0, "minSepMs": 200.0, "regions": [[0, n]]},
    ]
    return FixtureResult(mix, e)


def build_e_static_only():
    return _build_e_single("e_static_only", STATIC_KICK_VOICE, is_swept=False)


def build_e_sweep_only():
    return _build_e_single("e_sweep_only", SWEPT_KICK_VOICE, is_swept=True)


def build_f_overlap_streams():
    bpm = 120.0
    dur = 24.0
    interval = SR * 30.0 / bpm  # 12000, exact 8th
    offset_ms = 20.0
    offset_fr = ms_to_samples(offset_ms)
    pad = pad_frames_for(["x", "y"])
    n = int(round(dur * SR)) + pad
    mix = array('d', [0.0]) * n

    full_grid = grid_frames(interval, dur)
    x_frames = [full_grid[i] for i in range(0, len(full_grid), 2)]
    y_frames = [full_grid[i] + offset_fr for i in range(1, len(full_grid), 2)]

    for f in x_frames:
        place(mix, X_VOICE, f)
    for f in y_frames:
        place(mix, Y_VOICE, f)

    e = _new_entry(
        "120 BPM 8ths, 24 s, ONE continuous 8th-grid counter n=0,1,2,...; "
        "stream X (300 Hz, 30 ms decay) sounds on EVEN n exactly on grid, "
        "stream Y (350 Hz, 30 ms decay) sounds on ODD n at +20.0 ms late. "
        "X and Y therefore never overlap in TIME (they interleave every "
        "other 8th) but do occupy overlapping frequency bands.",
        "f_overlap_streams.wav", dur, pad, bpm=bpm)
    e["voiceLevels"] = {"x": 1.0, "y": 1.0}
    e["events"] = {"x": x_frames, "y": y_frames}
    e["groundTruth"] = {"deltaMuYMinusXMs": offset_ms, "freqX": X_FREQ, "freqY": Y_FREQ}
    e["verify"] = [
        {"name": "x", "band": "raw", "tolMs": 3.0, "minSepMs": 150.0, "regions": [[0, n]],
         "unionWith": ["y"]},
        {"name": "y", "band": "raw", "tolMs": 3.0, "minSepMs": 150.0, "regions": [[0, n]],
         "unionWith": ["x"]},
    ]
    return FixtureResult(mix, e)


def build_g_wash():
    bpm = 120.0
    dur = 16.0
    interval = SR * 30.0 / bpm
    offset_ms = 15.0
    offset_fr = ms_to_samples(offset_ms)
    pad = pad_frames_for(["low", "high"])
    n = int(round(dur * SR)) + pad
    mix = array('d', [0.0]) * n

    rng = random.Random(SEEDS["g_wash"])
    wash = wash_noise(n, rng)
    for i in range(n):
        mix[i] += wash[i]

    grid = grid_frames(interval, dur)
    high_frames = [f + offset_fr for f in grid]
    for f in grid:
        place(mix, LOW_VOICE, f)
    for f in high_frames:
        place(mix, HIGH_VOICE, f)

    e = _new_entry(
        "Same event content as a_offset15.wav (LOW on grid, HIGH +15.0 ms "
        "late, 120 BPM 8ths, 16 s) plus continuous WASH throughout at -12 dB "
        "relative to a click's pre-mix peak.",
        "g_wash.wav", dur, pad, bpm=bpm)
    e["voiceLevels"] = {"low": 1.0, "high": 1.0, "wash": WASH_LEVEL}
    e["events"] = {"low": grid, "high": high_frames}
    e["groundTruth"] = {"deltaMuHighMinusLowMs": offset_ms, "washLevelDb": WASH_DB}
    e["verify"] = [
        {"name": "low", "band": "low", "tolMs": 4.0, "minSepMs": 100.0, "regions": [[0, n]]},
        {"name": "high", "band": "high", "tolMs": 5.0, "minSepMs": 100.0, "regions": [[0, n]]},
    ]
    return FixtureResult(mix, e)


def build_h_fill_break():
    bpm = 120.0
    quarter = SR * 60.0 / bpm  # 24000
    bar_frames = 4 * quarter   # 96000 (2 s)
    interval = SR * 30.0 / bpm  # 12000, 8th

    n_normal1 = 8
    n_fill = 1
    n_silence = 2
    n_normal2 = 8

    normal1_end = n_normal1 * bar_frames
    fill_end = normal1_end + n_fill * bar_frames
    silence_end = fill_end + n_silence * bar_frames
    normal2_end = silence_end + n_normal2 * bar_frames
    total_bars = n_normal1 + n_fill + n_silence + n_normal2
    dur = total_bars * bar_frames / float(SR)

    pad = pad_frames_for(["low", "high", "broadband"])
    n = int(normal2_end) + pad
    mix = array('d', [0.0]) * n

    full_grid = grid_frames(interval, dur)
    low_frames = [f for f in full_grid if f < normal1_end or normal2_end - 1 <= f < normal2_end + 10**9]
    # explicit section membership (clearer than the filter above -- keep both,
    # assert they agree)
    low_frames = [f for f in full_grid if (f < normal1_end) or (f >= silence_end and f < normal2_end)]
    high_frames = list(low_frames)  # LOW+HIGH simultaneous through the normal sections

    for f in low_frames:
        place(mix, LOW_VOICE, f)
    for f in high_frames:
        place(mix, HIGH_VOICE, f)

    rng = random.Random(SEEDS["h_fill_break"])
    fill_count = 16
    fill_frames = _place_fill_hits(mix, rng, int(normal1_end), int(fill_end), fill_count)

    e = _new_entry(
        "120 BPM 4/4; LOW+HIGH simultaneous 8ths for 8 bars, then 1 bar of "
        "%d dense seeded random BROADBAND hits at non-grid positions, then "
        "2 bars of silence, then 8 bars normal again. The 8th-grid counter "
        "runs continuously through the fill/silence bars (so normal2 resumes "
        "in phase with normal1) but only sounds inside the normal sections." %
        fill_count,
        "h_fill_break.wav", dur, pad, bpm=bpm)
    e["voiceLevels"] = {"low": 1.0, "high": 1.0, "broadband": 1.0}
    e["events"] = {"low": low_frames, "high": high_frames, "fill": fill_frames}
    e["groundTruth"] = {
        "sections": [
            {"name": "normal1", "startFrame": 0, "endFrame": int(normal1_end)},
            {"name": "fill", "startFrame": int(normal1_end), "endFrame": int(fill_end)},
            {"name": "silence", "startFrame": int(fill_end), "endFrame": int(silence_end)},
            {"name": "normal2", "startFrame": int(silence_end), "endFrame": int(normal2_end)},
        ],
        "fillHitCount": fill_count,
        "barFrames": bar_frames,
    }
    e["verify"] = [
        {"name": "low", "band": "low", "tolMs": 4.0, "minSepMs": 100.0,
         "regions": [[0, int(normal1_end)], [int(silence_end), int(normal2_end)]]},
        {"name": "high", "band": "high", "tolMs": 3.0, "minSepMs": 100.0,
         "regions": [[0, int(normal1_end)], [int(silence_end), int(normal2_end)]]},
        {"name": "fill", "band": "raw", "tolMs": 6.0, "minSepMs": 15.0,
         "regions": [[int(normal1_end), int(fill_end)]]},
    ]
    e["silenceCheck"] = {"startFrame": int(fill_end), "endFrame": int(silence_end), "maxRms": 1e-4}
    return FixtureResult(mix, e)


def _place_fill_hits(mix, rng, start, end, count, min_gap=1200, margin=300):
    for _attempt in range(2000):
        pts = sorted(rng.uniform(start + margin, end - margin) for _ in range(count))
        ok = all(pts[i + 1] - pts[i] >= min_gap for i in range(len(pts) - 1))
        if ok:
            frames = [int(round(p)) for p in pts]
            for f in frames:
                place(mix, broadband_burst(rng), f)
            return frames
    raise RuntimeError("could not place %d fill hits with min gap %d in [%d,%d)" %
                        (count, min_gap, start, end))


def build_i_tempo_pair():
    bpm1, bpm2 = 120.0, 121.0
    dur1 = 16.0
    dur2 = 16.0
    interval1 = SR * 30.0 / bpm1  # 12000, exact
    interval2 = SR * 30.0 / bpm2  # not exact

    pad = pad_frames_for(["low", "high"])
    boundary = int(round(dur1 * SR))
    n = boundary + int(round(dur2 * SR)) + pad
    mix = array('d', [0.0]) * n

    phase1 = grid_frames(interval1, dur1)
    phase2_rel = grid_frames(interval2, dur2)
    phase2 = [boundary + f for f in phase2_rel]

    frames = phase1 + phase2
    for f in frames:
        place(mix, LOW_VOICE, f)
        place(mix, HIGH_VOICE, f)

    e = _new_entry(
        "16 s at exactly 120 BPM immediately followed by 16 s at exactly "
        "121 BPM, no gap (phase2's own grid restarts at the boundary frame); "
        "LOW+HIGH simultaneous 8ths throughout.",
        "i_tempo_pair.wav", dur1 + dur2, pad)
    e["voiceLevels"] = {"low": 1.0, "high": 1.0}
    e["events"] = {"low": frames, "high": frames,
                    "phase1": phase1, "phase2": phase2}
    e["groundTruth"] = {"boundaryFrame": boundary, "bpmPhase1": bpm1, "bpmPhase2": bpm2}
    e["verify"] = [
        {"name": "low", "band": "low", "tolMs": 3.0, "minSepMs": 80.0, "regions": [[0, n]]},
        {"name": "high", "band": "high", "tolMs": 3.0, "minSepMs": 80.0, "regions": [[0, n]]},
    ]
    return FixtureResult(mix, e)


BUILDERS = [
    ("a0_broadband_grid", build_a0_broadband_grid),
    ("a_offset15", build_a_offset15),
    ("b_jitter", build_b_jitter),
    ("c_tempo_drift", build_c_tempo_drift),
    ("d_twobar", build_d_twobar),
    ("e_sweep_vs_static", build_e_sweep_vs_static),
    ("e_static_only", build_e_static_only),
    ("e_sweep_only", build_e_sweep_only),
    ("f_overlap_streams", build_f_overlap_streams),
    ("g_wash", build_g_wash),
    ("h_fill_break", build_h_fill_break),
    ("i_tempo_pair", build_i_tempo_pair),
]


# ===========================================================================
# generation driver
# ===========================================================================


def generate(only=None):
    os.makedirs(OUTDIR, exist_ok=True)
    manifest = {"rate": SR, "fixtures": {}}
    total_bytes = 0
    for name, fn in BUILDERS:
        if only and name != only:
            continue
        result = fn()
        path = os.path.join(OUTDIR, result.manifest["file"])
        gain = write_wav(path, result.mixbuf)
        result.manifest["gainApplied"] = gain
        result.manifest["durationFrames"] = len(result.mixbuf)
        manifest["fixtures"][name] = result.manifest
        size = os.path.getsize(path)
        total_bytes += size
        print("wrote %-24s %8d frames  %7.3f s  gain=%.4f  %8d bytes" %
              (result.manifest["file"], len(result.mixbuf),
               len(result.mixbuf) / float(SR), gain, size))
    if only:
        # merge into existing manifest rather than clobbering the rest
        if os.path.exists(MANIFEST_PATH):
            with open(MANIFEST_PATH, "r") as f:
                existing = json.load(f)
            existing.setdefault("fixtures", {}).update(manifest["fixtures"])
            manifest = existing
    with open(MANIFEST_PATH, "w") as f:
        json.dump(manifest, f, indent=2, sort_keys=True)
    print("wrote %s" % MANIFEST_PATH)
    print("total wav bytes: %d (%.2f MB)" % (total_bytes, total_bytes / 1048576.0))
    return manifest


# ===========================================================================
# verification
# ===========================================================================


def bandsplit(x, window=48):
    n = len(x)
    prefix = [0.0] * (n + 1)
    for i in range(n):
        prefix[i + 1] = prefix[i] + x[i]
    half_lo = window // 2
    half_hi = window - half_lo
    low = array('d', [0.0]) * n
    for i in range(n):
        lo = i - half_lo
        hi = i + half_hi
        if lo < 0:
            lo = 0
        if hi > n:
            hi = n
        low[i] = (prefix[hi] - prefix[lo]) / (hi - lo)
    high = array('d', [0.0]) * n
    for i in range(n):
        high[i] = x[i] - low[i]
    return low, high


def block_energy(x, ds):
    n = len(x)
    nb = (n + ds - 1) // ds
    be = array('d', [0.0]) * nb
    for b in range(nb):
        s = b * ds
        e = min(n, s + ds)
        acc = 0.0
        for i in range(s, e):
            v = x[i]
            acc += v * v
        be[b] = acc / (e - s)
    return be


def moving_avg(a, window):
    n = len(a)
    prefix = [0.0] * (n + 1)
    for i in range(n):
        prefix[i + 1] = prefix[i] + a[i]
    out = array('d', [0.0]) * n
    half = window // 2
    for i in range(n):
        lo = max(0, i - half)
        hi = min(n, i + (window - half))
        out[i] = (prefix[hi] - prefix[lo]) / (hi - lo)
    return out


def detect_events(signal, ds, regions, min_sep_frames, thresh_factor=5.0,
                   abs_floor=1e-7, baseline_win_ms=150.0, rel_floor=0.02):
    """Blind peak-picker over short-time block energy. The threshold is the
    max of three floors: a local adaptive baseline (`baseline_win_ms`
    moving average * thresh_factor -- catches an event against a raised
    noise floor, e.g. WASH), an absolute floor (catches an event in near
    silence), and a RELATIVE floor (`rel_floor` * the loudest block energy
    seen anywhere in the allowed regions). The relative floor is what keeps
    a band-split filter's edge-transient leakage from another band's sharp
    attack (e.g. a LOW click's 0.5 ms ramp bleeding a small residual into
    the "high" band) from reading as a real same-band event: that leakage
    is two to three orders of magnitude below a genuine same-band burst's
    energy, so a single global-peak-relative cutoff separates them cleanly
    without needing a per-fixture tuned constant."""
    n = len(signal)
    be = block_energy(signal, ds)
    nb = len(be)
    baseline_win_blocks = max(3, int(round(baseline_win_ms / 1000.0 * SR / ds)))
    baseline = moving_avg(be, baseline_win_blocks)

    allowed = [False] * nb
    for (s, e) in regions:
        bs = max(0, s // ds)
        be_end = min(nb, (e + ds - 1) // ds)
        for b in range(bs, be_end):
            allowed[b] = True

    global_peak = max((be[b] for b in range(nb) if allowed[b]), default=0.0)
    rel_thr = rel_floor * global_peak

    cand = []
    for b in range(nb):
        if not allowed[b]:
            continue
        thr = max(baseline[b] * thresh_factor + abs_floor, rel_thr)
        if be[b] > thr:
            cand.append(b)

    min_sep_blocks = max(1, int(round(min_sep_frames / float(ds))))
    cand.sort(key=lambda b: -be[b])
    picked = []
    for b in cand:
        ok = True
        for p in picked:
            if abs(b - p) < min_sep_blocks:
                ok = False
                break
        if ok:
            picked.append(b)
    picked.sort()

    refined = []
    for b in picked:
        center = b * ds + ds // 2
        lo = max(0, center - ds * 2)
        hi = min(n, center + ds * 2)
        best_i, best_v = lo, -1.0
        for i in range(lo, hi):
            v = signal[i] * signal[i]
            if v > best_v:
                best_v = v
                best_i = i
        refined.append(best_i)
    return refined


def match_events(expected, detected, tol_frames, match_window_factor=4.0,
                  also_ok=()):
    """Match `detected` frames to `expected` by nearest neighbour within a
    generous window. Leftover (unmatched) detections are then checked
    against `also_ok` -- frames belonging to ANOTHER stream that legitimately
    shares the same search region (e.g. f_overlap_streams' X and Y interleave
    in time) -- and only what remains after THAT is counted as `extra`."""
    detected_sorted = sorted(detected)
    used = [False] * len(detected_sorted)
    residuals = []
    missing = 0
    window = tol_frames * match_window_factor
    for exp in expected:
        best_j = -1
        best_d = None
        for j, dv in enumerate(detected_sorted):
            if used[j]:
                continue
            d = abs(dv - exp)
            if best_d is None or d < best_d:
                best_d = d
                best_j = j
        if best_j >= 0 and best_d <= window:
            used[best_j] = True
            residuals.append(detected_sorted[best_j] - exp)
        else:
            missing += 1
    if also_ok:
        for j, dv in enumerate(detected_sorted):
            if used[j]:
                continue
            for other in also_ok:
                if abs(dv - other) <= window:
                    used[j] = True
                    break
    extra = used.count(False)
    return residuals, missing, extra


Check = namedtuple("Check", "fixture name expected measured tol ok note")


def _run_generic_stream_checks(name, entry, x, low, high, results):
    n = len(x)
    for stream in entry.get("verify", []):
        band = stream["band"]
        if band == "low":
            sig = low
        elif band == "high":
            sig = high
        else:
            sig = x
        match_key = stream.get("matchAgainst", stream["name"])
        expected = stream.get("framesOverride")
        if expected is None:
            expected = entry["events"][match_key]
        also_ok = []
        for other_key in stream.get("unionWith", []):
            also_ok.extend(entry["events"][other_key])
        tol_ms = stream["tolMs"]
        tol_frames = tol_ms * SR / 1000.0
        min_sep = stream["minSepMs"] * SR / 1000.0
        regions = stream["regions"]
        detected = detect_events(sig, ds=64, regions=regions, min_sep_frames=min_sep)
        residuals, missing, extra = match_events(expected, detected, tol_frames,
                                                   also_ok=also_ok)
        max_abs_ms = (max((abs(r) for r in residuals), default=0.0)) * 1000.0 / SR
        ok = (missing == 0) and (extra == 0) and (max_abs_ms <= tol_ms)
        note = ("n=%d matched=%d missing=%d extra=%d maxAbsResidualMs=%.3f" %
                (len(expected), len(residuals), missing, extra, max_abs_ms))
        results.append(Check(name, stream["name"], len(expected),
                              len(detected), tol_ms, ok, note))


def verify(only=None):
    if not os.path.exists(MANIFEST_PATH):
        print("no manifest at %s -- run the generator first" % MANIFEST_PATH)
        return False
    with open(MANIFEST_PATH, "r") as f:
        manifest = json.load(f)

    results = []
    for name, entry in sorted(manifest["fixtures"].items()):
        if only and name != only:
            continue
        path = os.path.join(OUTDIR, entry["file"])
        if not os.path.exists(path):
            results.append(Check(name, "file", "exists", "MISSING", 0, False, path))
            continue
        x = read_wav_mono_float(path)
        low, high = bandsplit(x)

        _run_generic_stream_checks(name, entry, x, low, high, results)

        if "silenceCheck" in entry:
            sc = entry["silenceCheck"]
            s, e = sc["startFrame"], sc["endFrame"]
            seg = x[s:e]
            rms = math.sqrt(sum(v * v for v in seg) / len(seg)) if seg else 0.0
            ok = rms <= sc["maxRms"]
            results.append(Check(name, "silence", "<=%.6g" % sc["maxRms"],
                                  "%.6g" % rms, sc["maxRms"], ok,
                                  "rms over [%d,%d)" % (s, e)))

        if name == "b_jitter":
            segs = entry["groundTruth"]["segments"]
            gaussian = [s for s in segs if s["kind"] == "gaussian"]
            stds = [s["measuredStdMs"] for s in gaussian]
            mono_ok = all(stds[i] <= stds[i + 1] + 0.75 for i in range(len(stds) - 1))
            results.append(Check(name, "jitter sigma monotonic",
                                  "non-decreasing (tol 0.75ms)",
                                  ["%.2f" % v for v in stds], 0.0, mono_ok,
                                  "gaussian segment measured std, ms"))
            ar1 = [s for s in segs if s["kind"] == "ar1"]
            ar1_band_ok = all(4.0 <= s["measuredStdMs"] <= 22.0 for s in ar1)
            results.append(Check(name, "ar1 sigma in band",
                                  "[4,22] ms (target 12)",
                                  ["%.2f" % s["measuredStdMs"] for s in ar1],
                                  0.0, ar1_band_ok, "ar1 segment measured std, ms"))
            lags = [s["measuredLag1"] for s in ar1]
            lag_mono_ok = all(
                (lags[i] is None or lags[i + 1] is None or lags[i] <= lags[i + 1] + 0.15)
                for i in range(len(lags) - 1)
            )
            results.append(Check(name, "ar1 lag-1 autocorr rises with tau",
                                  "non-decreasing (tol 0.15)",
                                  ["%.3f" % (v if v is not None else float('nan')) for v in lags],
                                  0.0, lag_mono_ok, "lag-1 autocorrelation per ar1 segment"))

    ok_all = all(c.ok for c in results)
    _print_table(results)
    return ok_all


def _print_table(results):
    col1 = max(8, max((len(c.fixture) for c in results), default=8))
    col2 = max(6, max((len(c.name) for c in results), default=6))
    header = "%-*s  %-*s  %-6s  %s" % (col1, "fixture", col2, "check", "result", "note")
    print(header)
    print("-" * len(header))
    n_pass = 0
    for c in results:
        status = "PASS" if c.ok else "FAIL"
        if c.ok:
            n_pass += 1
        print("%-*s  %-*s  %-6s  %s" % (col1, c.fixture, col2, c.name, status, c.note))
    print("-" * len(header))
    print("%d/%d checks passed" % (n_pass, len(results)))


# ===========================================================================
# CLI
# ===========================================================================


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--verify", action="store_true",
                     help="verify existing fixtures instead of generating them")
    ap.add_argument("--only", default=None,
                     help="restrict to one fixture name (for fast iteration)")
    args = ap.parse_args()

    if args.verify:
        ok = verify(only=args.only)
        sys.exit(0 if ok else 1)
    else:
        generate(only=args.only)


if __name__ == "__main__":
    main()
