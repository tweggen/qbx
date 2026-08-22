#!/usr/bin/env python3
"""Generator for tests/test_gapsaw.wav -- the TAKE-LANE DOMAIN fixture
(qxa.take_lane_domain).

NOT A TEST. It lives outside tests/cases/ so the CONFIGURE_DEPENDS glob never
registers it, and it is committed for the same reason gen_auto_fixture.py and
gen_clip_fixture.py are: a fixture nobody can regenerate is a fixture nobody
can reason about.

    cd smaragd/tests/tools && python gen_gap_fixture.py

WHY A NEW FIXTURE.

  The thing under test is WHICH PART of a take a take lane draws. That needs
  landmarks in the material that a reader can point at, and NO committed
  fixture has any:

    * test_sawtooth / test_autosaw / test_clipsaw / test_position /
      test_stereo / test_channels4 are all CONTINUOUS -- every column of every
      window carries material, so "where does material start" is the clip's
      left edge whatever window is drawn, and a wrong window is invisible to a
      position measurement.
    * The obvious dodge -- window PAST the end of the source and look for the
      silence -- does not work either: SObject::getStraightPreview CLAMPS its
      probe index to the last probe (sobject.cpp), so a window that overhangs
      draws the last level HELD, not a gap.

  So the landmarks have to be in the audio. This fixture is a ramp (level
  still encodes position, as test_sawtooth's does) with two EXACT-ZERO regions
  at DELIBERATELY ASYMMETRIC positions.

THE ASYMMETRY IS THE WHOLE POINT.

  qxa.take_lane_domain places a 2 s wrapper window at 2 s into a 4 s take:

    correct : the lane shows source [2.0, 4.0) -> the [2.5, 3.0) silence, i.e.
              a gap at 25%..50% of the clip's pixel span
    broken  : the lane shows source [0.0, 2.0) -> the [0.6, 0.9) silence, i.e.
              a gap at 30%..45%

  Both windows contain EXACTLY ONE gap, so the case cannot pass by accident on
  gap PRESENCE -- it has to get the POSITION right. A periodic silence pattern
  (say every 0.5 s) would have produced the same relative positions in both
  windows and gated nothing, which is the trap this comment exists to record.

GEOMETRY.

  4.0 s, 48 kHz, stereo, 16-bit PCM = 192000 frames.
  Sawtooth period EXACTLY 100 frames (480 Hz), so every window boundary the
  case uses lands on a cycle boundary and the RMS of any span is a closed
  form -- the same trick test_autosaw.wav uses.
  Peak amplitude ramps linearly 0.1 -> 0.8 across the 4 s.
  Both channels carry the SAME samples: this fixture makes no channel claim
  (see CLAUDE.md on why test_sawtooth.wav cannot gate one either).
"""

import os
import struct

RATE     = 48000
CHANNELS = 2
DURATION = 4.0
PERIOD   = 100                      # frames; 480 Hz, divides 48000 exactly
AMP0     = 0.1
AMP1     = 0.8
# Exact-zero regions, in seconds. ASYMMETRIC on purpose -- see the docstring.
SILENCES = [ ( 0.6, 0.9 ), ( 2.5, 3.0 ) ]

OUT = os.path.join( os.path.dirname( os.path.abspath( __file__ ) ),
                    "..", "test_gapsaw.wav" )


def main():
    n = int( RATE * DURATION )
    silent = [ ( int( a * RATE ), int( b * RATE ) ) for a, b in SILENCES ]

    frames = bytearray()
    for i in range( n ):
        if any( lo <= i < hi for lo, hi in silent ):
            s = 0
        else:
            # Sawtooth: -1 .. +1 across one period, scaled by the ramp.
            phase = ( i % PERIOD ) / float( PERIOD )
            amp   = AMP0 + ( AMP1 - AMP0 ) * ( i / float( n ) )
            s     = int( round( ( 2.0 * phase - 1.0 ) * amp * 32767.0 ) )
            s     = max( -32768, min( 32767, s ) )
        frames += struct.pack( "<h", s ) * CHANNELS

    data = bytes( frames )
    byte_rate   = RATE * CHANNELS * 2
    block_align = CHANNELS * 2
    hdr = b"RIFF" + struct.pack( "<I", 36 + len( data ) ) + b"WAVEfmt " \
        + struct.pack( "<IHHIIHH", 16, 1, CHANNELS, RATE, byte_rate,
                       block_align, 16 ) \
        + b"data" + struct.pack( "<I", len( data ) )

    with open( OUT, "wb" ) as f:
        f.write( hdr )
        f.write( data )

    print( "wrote %s: %d frames, %.1f s, %d ch, %d Hz"
           % ( os.path.normpath( OUT ), n, DURATION, CHANNELS, RATE ) )
    for a, b in SILENCES:
        print( "  exact silence [%.3f, %.3f) s = frames [%d, %d)"
               % ( a, b, int( a * RATE ), int( b * RATE ) ) )


if __name__ == "__main__":
    main()
