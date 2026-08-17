#!/usr/bin/env python3
"""Generator for tests/test_autosaw.wav -- the AUTOMATION fixture of proposal
37 P5 (qxa.automation_volume_ramp / _mute_step / _plugin_param / _clip_gain /
_edit_invalidates).

NOT A TEST. It lives outside tests/cases/ so the CONFIGURE_DEPENDS glob never
registers it, and it is committed for the same reason gen_clip_fixture.py is: a
fixture nobody can regenerate is a fixture nobody can reason about.

    cd smaragd/tests/tools && python gen_auto_fixture.py

WHY A NEW FIXTURE.

  * test_sawtooth.wav RAMPS in level, so its per-second RMS is a different
    number every second -- useless when the thing under test is a per-second
    level ramp.
  * test_clipsaw.wav is constant but only 2.0 s long, and its 100 Hz period is
    480 frames, so the 100-frame windows the MUTE ramp needs (the ~2 ms either
    side of a transition) contain a fifth of a cycle and their RMS depends
    entirely on where in the cycle they land.

WHAT IT IS. 4.0 s, 48 kHz, 16-bit PCM, two identical channels, a 480 Hz
sawtooth of amplitude 0.4:

    x[n] = 0.4 * (2 * ((n mod 100) / 100) - 1)

The period is EXACTLY 100 frames, which is what every assertion in the five
cases leans on:

  * 100 divides 48000, so every one-SECOND window has the same RMS (the AC1
    ramp and the AC5 step bands).
  * 100 divides 70000, so the [69000, 70000) / [70000, 71000) windows AC3
    measures either side of a mid-chunk parameter step are whole cycles.
  * 100 frames IS ~2.083 ms, so the window AC2 measures at each mute edge is
    one whole cycle -- and the 1.5 ms (72-frame) gain-stage ramp sits inside it.

Amplitude 0.4, not 0.95: AC3 runs the material through a gain of 2.0, and 0.8
peak leaves the 16-bit write unclipped.
"""

import math
import os
import struct
import wave

SR = 48000
SECONDS = 4.0
FREQ = 480
AMPL = 0.4
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "test_autosaw.wav")

PERIOD = SR // FREQ          # 100, exact
NFRAMES = int(SR * SECONDS)


def samples():
    for i in range(NFRAMES):
        x = AMPL * (2.0 * ((i % PERIOD) / PERIOD) - 1.0)
        yield max(-32768, min(32767, int(round(x * 32767.0))))


def rms(vals):
    if not vals:
        return 0.0
    return math.sqrt(sum(v * v for v in vals) / len(vals))


def main():
    data = list(samples())
    w = wave.open(OUT, "wb")
    w.setnchannels(2)
    w.setsampwidth(2)
    w.setframerate(SR)
    w.writeframes(struct.pack("<%dh" % (len(data) * 2),
                              *[v for v in data for _ in range(2)]))
    w.close()

    # Read back exactly the way the engine does: int16 / 32768.
    f = [v / 32768.0 for v in data]
    base = rms(f[:SR])
    print("wrote %s  (%d frames, %.1f s)" % (OUT, NFRAMES, SECONDS))
    print("peak                       %.6f" % max(abs(v) for v in f))
    print("rms, any 100-aligned win   %.6f" % base)
    for k in range(4):
        print("  rms, second %d            %.6f" % (k, rms(f[k * SR:(k + 1) * SR])))

    # --- AC1: self:Volume, LINEAR IN dB from -60 dB at 0 s to 0 dB at 4 s ----
    print("\nAC1  volume ramp -60 dB -> 0 dB over [0, 192000), linear in dB")
    for k in range(4):
        seg = []
        for i in range(k * SR, (k + 1) * SR):
            db = -60.0 + 60.0 * (i / float(NFRAMES))
            seg.append(f[i] * (10.0 ** (db / 20.0)))
        r = rms(seg)
        print("  second %d rms %.8f   -1%%..+1%% [%.8f, %.8f]"
              "   -3%%..+3%% [%.8f, %.8f]"
              % (k, r, r * 0.99, r * 1.01, r * 0.97, r * 1.03))
    mid = -60.0 + 60.0 * (2 * SR / float(NFRAMES))
    print("  value at frame %d = %.6f dB" % (2 * SR, mid))

    # --- AC2: self:Muted, on at 1 s, off at 2 s, 72-frame (1.5 ms) ramps ----
    RAMP = SR * 3 // 2000        # twGainStage::muteRampFrames(), 72
    print("\nAC2  mute step, ramp = %d frames (%.3f ms)" % (RAMP, RAMP * 1000.0 / SR))

    def mute_factor(i):
        # audible until 48000, muted until 96000, audible after
        if i < SR:
            return 1.0
        if i < 2 * SR:
            d = i - SR
            return max(0.0, 1.0 - d / float(RAMP))
        d = i - 2 * SR
        return min(1.0, d / float(RAMP))

    win = 100
    for name, start in (("mute-on  edge", SR), ("mute-off edge", 2 * SR)):
        seg = [f[i] * mute_factor(i) for i in range(start, start + win)]
        r = rms(seg)
        print("  %s [%d, %d)  rms %.8f  (base %.8f)   bands 0.75x..1.25x "
              "[%.8f, %.8f]" % (name, start, start + win, r, base,
                                r * 0.75, r * 1.25))
    seg = [f[i] * mute_factor(i) for i in range(SR + 96, 2 * SR - 96)]
    print("  muted body [%d, %d) rms %.10f" % (SR + 96, 2 * SR - 96, rms(seg)))

    # --- AC3: param step 1.0 -> 2.0 at frame 70000 ---------------------------
    print("\nAC3  plugin param step at frame 70000")
    a = rms(f[69000:70000])
    b = rms([v * 2.0 for v in f[70000:71000]])
    print("  [69000,70000) rms %.8f   +/-1%% [%.8f, %.8f]" % (a, a * 0.99, a * 1.01))
    print("  [70000,71000) rms %.8f   +/-1%% [%.8f, %.8f]" % (b, b * 0.99, b * 1.01))

    # --- AC5: step lane 0 / -12 / 0 dB --------------------------------------
    print("\nAC5  step lane 0 dB / -12 dB (second 2) / 0 dB")
    g12 = 10.0 ** (-12.0 / 20.0)
    g6 = 10.0 ** (-6.0 / 20.0)
    print("  second 2 at -12 dB rms %.8f  +/-2%% [%.8f, %.8f]"
          % (base * g12, base * g12 * 0.98, base * g12 * 1.02))
    print("  second 2 at  -6 dB rms %.8f  +/-2%% [%.8f, %.8f]"
          % (base * g6, base * g6 * 0.98, base * g6 * 1.02))
    print("  second 0/1/3 at 0 dB rms %.8f" % base)

    # --- AC4: cut:Gain linear fade 1.0 -> 0.0 over [0, 192000) --------------
    print("\nAC4  clip gain envelope, LINEAR 1.0 -> 0.0 over the 4 s clip")
    for k in range(4):
        seg = [f[i] * (1.0 - i / float(NFRAMES)) for i in range(k * SR, (k + 1) * SR)]
        r = rms(seg)
        print("  second %d rms %.8f   +/-3%% [%.8f, %.8f]"
              % (k, r, r * 0.97, r * 1.03))


if __name__ == "__main__":
    main()
