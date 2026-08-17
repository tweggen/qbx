#!/usr/bin/env python3
"""Generator for tests/test_clipsaw.wav -- the ORDER-SENSITIVE fixture of
proposal 37 P3a (qxa.fader_post_fx, case (b)).

NOT A TEST. It lives outside tests/cases/ so the CONFIGURE_DEPENDS glob never
registers it, and it is committed for the same reason gen_mc_corpus.qxa is: a
fixture nobody can regenerate is a fixture nobody can reason about.

    cd smaragd/tests/tools && python gen_clip_fixture.py

WHY A NEW FIXTURE AND NOT test_sawtooth.wav. The order case needs a signal that
a fader at -6.02 dB leaves BELOW a 0.5 clip threshold and that the raw signal is
ABOVE it, so that "fader then clipper" and "clipper then fader" produce
different audio. That means a peak in [0.5, 1.0), and comfortably inside it:
test_sawtooth.wav's loudest second peaks at 0.798, which works but leaves only
11 % between the two answers. At 0.95 the gap is 27 %.

WHAT IT IS. 2.0 s, 48 kHz, 16-bit PCM, two identical channels, a 100 Hz
sawtooth of amplitude 0.95:

    x[n] = 0.95 * (2 * ((n mod 480) / 480) - 1)

480 samples per period divides 48000 exactly, so every second contains a whole
number of periods and the RMS of ANY one-second window is the same number. That
is what makes the assertion in the .qxa a closed form rather than a measurement
of one particular window.
"""

import math
import os
import struct
import wave

SR = 48000
SECONDS = 2.0
FREQ = 100
AMPL = 0.95
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "test_clipsaw.wav")


def samples():
    period = SR // FREQ  # 480, exact
    n = int(SR * SECONDS)
    for i in range(n):
        x = AMPL * (2.0 * ((i % period) / period) - 1.0)
        yield max(-32768, min(32767, int(round(x * 32767.0))))


def main():
    data = list(samples())
    w = wave.open(OUT, "wb")
    w.setnchannels(2)
    w.setsampwidth(2)
    w.setframerate(SR)
    w.writeframes(struct.pack("<%dh" % (len(data) * 2),
                              *[v for v in data for _ in range(2)]))
    w.close()

    # The numbers quoted in fader_post_fx.qxa, printed so they can be re-derived.
    f = [v / 32768.0 for v in data[:SR]]
    rms = math.sqrt(sum(v * v for v in f) / len(f))
    clipped = [max(-0.5, min(0.5, v)) for v in f]
    crms = math.sqrt(sum(v * v for v in clipped) / len(clipped))
    print("wrote %s (%d frames)" % (os.path.normpath(OUT), len(data)))
    print("  peak                       %.6f" % max(abs(v) for v in f))
    print("  rms (1 s)                  %.6f" % rms)
    print("  pre-FX  order 0.5*rms      %.6f" % (0.5 * rms))
    print("  post-FX order 0.5*rms(clip) %.6f" % (0.5 * crms))


if __name__ == "__main__":
    main()
