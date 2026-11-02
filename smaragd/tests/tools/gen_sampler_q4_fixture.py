#!/usr/bin/env python3
"""Generator for tests/sampler_q4_loop.wav -- the 2-beat loop of proposal 56
Q4 (QBX-268: tempo modes by stretching; nassau-digital GATES Q4).

NOT A TEST. It lives outside tests/cases/ so the CONFIGURE_DEPENDS glob never
registers it, and it is committed so the fixture can be regenerated and
reasoned about.

    cd smaragd/tests/tools && python3 gen_sampler_q4_fixture.py

THE SIGNAL: exactly 1 s at 48 kHz (48000 frames) = 2 beats at 120 BPM, mono,
16-bit PCM. A 1 kHz sine carrier (exactly 1000 cycles, so the loop's seam is
continuous in phase) under four sharp-attack hits per loop, one per eighth
note (0, 0.25, 0.5, 0.75 s): each hit jumps to 0.12 + 0.38 * accent and decays
with a 40 ms time constant towards 0.12; the first hit is accented (1.0), the
others are not (0.65).

  * The carrier gives the PITCH (Q4.1: unchanged +- 5 cents by stretch).
  * The accent makes the loop periodic with the LOOP's period and nothing
    shorter, so an autocorrelation over 1.15..1.25 s finds the loop (1.2 s at
    100 BPM) and not a multiple of the 0.3 s hit spacing.
  * The attacks are TRANSIENTS the onset analysis finds (twDetectOnsets), so
    a stretch with onset keyframes differs from one on the vocoder's fixed
    grid (Q4.2: the analysis's completion is a new generation that sounds
    different).
"""
import math
import struct
import wave
import os

RATE = 48000
N = RATE  # 1 s
HIT = RATE // 4  # an eighth note at 120 BPM

frames = []
for i in range(N):
    k, local = divmod(i, HIT)
    accent = 1.0 if k == 0 else 0.65
    env = 0.12 + 0.38 * accent * math.exp(-(local / RATE) / 0.040)
    v = env * math.sin(2.0 * math.pi * 1000.0 * i / RATE)
    frames.append(max(-32768, min(32767, int(round(v * 32767.0)))))

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'sampler_q4_loop.wav')
with wave.open(out, 'wb') as w:
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(RATE)
    w.writeframes(b''.join(struct.pack('<h', f) for f in frames))
print('wrote', os.path.normpath(out), N, 'frames')
