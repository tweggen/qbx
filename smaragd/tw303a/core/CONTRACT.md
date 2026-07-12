# tw/core — CONTRACT

Purpose: the bottom of the dependency graph — value types and pure utilities
every other module builds on. Nothing here knows about components, pages,
files, or the app.

Public headers: twtypes.h (sample_t/offset_t/length_t/idx_t, preview_t,
SAMPLE_NORM_*), twformat.h (twFormat + capability domains), twfraction.h
(exact rational arithmetic for positions), twconvert.h (twConvertFrames —
format conversion, NO rate change), audio_frame.h (AudioFrame — shared
currency of playback pullFrame and sinks writeFrame), generation_promise.h
(std::future-based generation gate), exc.h, twsyslog.h.

Depends on: QtCore only. Forbidden: every other tw/ module, anything app/.

Threading: everything is a value type or stateless function except
GenerationPromise, which is internally synchronized.

Invariants:
1. This module includes NOTHING from the rest of the project.
2. twConvertFrames converts sample type/channels only; sample-RATE conversion
   is tw/sources (twResampler / viewAtRate) — never add it here.
3. Fraction is exact: position math that must round-trip through project
   files goes through Fraction, not double (see proposal 04).

How to test: build/bin/exact_arithmetic_test.exe and
serialization_roundtrip_test.exe (link only tw_core).

Known debt: QtCore dependency (QString in twfraction.cpp tail); SAMPLE_NORM_*
are macros; DTOR_DEL macro survives for legacy call sites.
