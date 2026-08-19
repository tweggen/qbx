#!/usr/bin/env python3
"""
Generate tests/test_sawtooth_id3.mp3 from tests/test_sawtooth.mp3 by prepending
an ID3v2.3 tag that carries the frames mpg123 1.33.4 hangs on.

WHY THIS FIXTURE HAS TO EXIST AS ITS OWN FILE. test_sawtooth.mp3 carries no ID3
tag at all, so it can never exercise the tag path -- it opens identically on a
fixed and a broken build. The three frame types below are exactly the ones whose
mpg123 handlers call strcasecmp/strncasecmp, which on a Windows/MinGW -O3 build
of mpg123 compile to a literal self-jump (see twsamplesource.cc for the full
mechanism). USLT, APIC and plain text frames do NOT reproduce it, so a tag made
only of those would be a fixture that never fails.

The audio payload is byte-identical to test_sawtooth.mp3, so the new case can
assert the same RMS band the existing mp3_sample_import case does: any
difference between the two renders is the tag handling and nothing else.

Regenerate with:  python3 tests/tools/gen_id3_fixture.py
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TESTS = os.path.dirname(HERE)
SRC = os.path.join(TESTS, "test_sawtooth.mp3")
DST = os.path.join(TESTS, "test_sawtooth_id3.mp3")


def syncsafe(n):
    return bytes([(n >> 21) & 0x7F, (n >> 14) & 0x7F, (n >> 7) & 0x7F, n & 0x7F])


def frame(fid, payload):
    """One ID3v2.3 frame: 4-byte id, 4-byte big-endian size, 2 flag bytes."""
    return fid + struct.pack(">I", len(payload)) + b"\x00\x00" + payload


def main():
    audio = open(SRC, "rb").read()
    if audio[:3] == b"ID3":
        sys.exit("%s already carries an ID3 tag; expected a bare stream" % SRC)

    frames = b"".join([
        # Text encoding 0 (Latin-1), language 'eng', description, then the text.
        # A description terminator FOLLOWED BY TEXT is what trips the bug; a
        # description with nothing after it does not.
        frame(b"COMM", b"\x00" + b"eng" + b"iTunNORM\x00" + b"0000030B 000002A7"),
        frame(b"TXXX", b"\x00" + b"replaygain_track_gain\x00" + b"-3.21 dB"),
        # RVA2: null-terminated identification, then channel/volume/peak.
        frame(b"RVA2", b"track\x00" + bytes([1]) + b"\x00\x10" + bytes([16]) + b"\x80\x00"),
        # One frame type that is handled but does NOT reproduce, so the fixture
        # also proves the skip does not depend on the tag being uniformly bad.
        frame(b"TIT2", b"\x00" + b"tw sawtooth id3 fixture"),
    ])
    tag = b"ID3" + bytes([3, 0, 0]) + syncsafe(len(frames)) + frames

    with open(DST, "wb") as f:
        f.write(tag + audio)
    print("wrote %s: %d byte tag + %d byte audio = %d bytes"
          % (DST, len(tag), len(audio), len(tag) + len(audio)))


if __name__ == "__main__":
    main()
