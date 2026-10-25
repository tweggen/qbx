#!/usr/bin/env python3
"""Generator for tests/test_clicks_head_blip.wav -- the GRID-ANCHOR fixture
(qxa.metronome_click_while_recording, QBX-227).

NOT A TEST. It lives outside tests/cases/ so the CONFIGURE_DEPENDS glob never
registers it, and it is committed for the same reason gen_gap_fixture.py is: a
fixture nobody can regenerate is a fixture nobody can reason about.

    cd smaragd/tests/tools && python gen_clicks_head_blip_fixture.py

WHY A NEW FIXTURE.

  `assert-metronome-clicks` used to anchor its beat grid on the FIRST detected
  onset. Under load an EXTRA onset appears near the capture head -- the first
  click of a live lane sits inside the RT's fade-in ramp, so the running
  window crosses `threshold` wherever the ramp happened to reach it -- and the
  grid then rested on a point the capture start placed rather than the tempo
  map. Measured on a failing and a passing run of the same binary at loadavg
  ~14:

      FAILED  0@0   (pk 0.2109), 1@25986, 2@49986, 3@73986, 4@97985, ...
      PASSED  0@995 (pk 0.2109), 1@25986, 2@49986, 3@73986, 4@97985, ...

  Onsets 1.. are an exact 24000 grid in both and sit at the SAME absolute
  frames; only onset 0 moves, and its own distance from that grid is 991 in
  one run and 1986 in the other, against a 1024 tolerance.

  No live case can gate the remedy, because whether the head onset appears at
  all is the box's business -- an idle run produces eight onsets and no head
  artifact. So the shape goes into a FIXTURE, where it is bytes:

      onset 0     at frame 0, peak 0.2109   <- the head artifact, OFF the grid
      onsets 1..8 at 25986 + k*24000, peak 0.3500   <- an exact grid

  Anchored on onset 0 that reads a worst grid error of 1930, over the 1024
  bound; anchored on onset 1 it reads 0. The detector reports the onsets 56
  frames early (the 64-frame window reaches `threshold` before the nominal
  start), which is why the measured onsets are 0 and 25930 rather than 0 and
  25986; only the SPACING matters to the grid.

MONO, deliberately: the verb reads one channel, and a committed binary should
not carry a duplicate of it.
"""

import math
import struct
import wave

SR    = 48000
CLICK = 960            # 20 ms, as the real metronome click
GRID0 = 25986          # where the real clicks start, from the measurement
IVAL  = 24000          # 120 BPM quarter notes at 48 kHz
N     = 9              # one head artifact plus eight grid clicks
HEAD_PEAK = 0.2109     # the attenuated head onset, measured
GRID_PEAK = 0.3500     # a full click, measured
TOTAL = GRID0 + (N - 2) * IVAL + CLICK + IVAL


def click(buf, at, peak):
    """A short decaying 1 kHz burst, the shape of the metronome's own."""
    for i in range(CLICK):
        if at + i >= len(buf):
            break
        env = math.exp(-4.0 * i / CLICK)
        buf[at + i] = peak * env * math.sin(2.0 * math.pi * 1000.0 * i / SR)


def main():
    buf = [0.0] * TOTAL
    click(buf, 0, HEAD_PEAK)
    for k in range(1, N):
        click(buf, GRID0 + (k - 1) * IVAL, GRID_PEAK)

    out = "../test_clicks_head_blip.wav"
    w = wave.open(out, "wb")
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(SR)
    w.writeframes(b"".join(
        struct.pack("<h", max(-32768, min(32767, int(round(v * 32767)))))
        for v in buf))
    w.close()
    print("wrote %s: %d frames (%.2f s), mono" % (out, TOTAL, TOTAL / SR))


main()
