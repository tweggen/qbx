# tw/core — CONTRACT

Purpose: the bottom of the dependency graph — value types and pure utilities
every other module builds on. Nothing here knows about components, pages,
files, or the app.

Public headers: twtypes.h (sample_t/offset_t/length_t/idx_t, preview_t,
SAMPLE_NORM_*), twformat.h (twFormat + capability domains), twfraction.h
(exact rational arithmetic for positions), twconvert.h (twConvertFrames —
format conversion, NO rate change), audio_frame.h (AudioFrame — shared
currency of playback pullFrame and sinks writeFrame), generation_promise.h
(std::future-based generation gate), exc.h, twsyslog.h, twlog.h (TwLog — the
process-wide log sink and the TW_LOG* macros; proposal 24).

Depends on: QtCore only. Forbidden: every other tw/ module, anything app/.

Threading: everything is a value type or stateless function except
GenerationPromise and TwLog, both internally synchronized. TwLog is callable
from any thread including the RT audio callback (see invariant 5).

Invariants:
1. This module includes NOTHING from the rest of the project.
2. twConvertFrames converts sample type/channels only; sample-RATE conversion
   is tw/sources (twResampler / viewAtRate) — never add it here.
3. Fraction is exact: position math that must round-trip through project
   files goes through Fraction, not double (see proposal 04).
4. TwLog is plain C++ — no QObject, no signals, no Qt types on its producer
   path. Engine and worker threads log through it, and they must never touch
   Qt (docs/contracts/THREADING.md).
5. A thread that calls TwLog::markNonBlocking() (the RT audio callback) never
   waits: it truncates at TW_LOG_RT_MAX and try_locks, counting a drop.

**Toolchain constraint — no `thread_local` with a non-trivial destructor.**
On MinGW-w64 GCC 13.1.0 (x86_64-posix-seh, the Windows build compiler), a
`thread_local std::string`/`std::vector`/anything needing `__cxa_thread_atexit`
corrupts the heap once ~3+ threads touch it — STATUS_HEAP_CORRUPTION
(0xc0000374), reproduced 10/10 in an 8-thread program with no project code
linked in. POD thread_locals (bool, integers, pointers, plain arrays) are fine
and are what twlog.cc and tw_freeze_context.h use. This applies to the whole
codebase, not just this module.

How to test: `ctest -R 'exact_arithmetic|serialization_roundtrip|twfraction|timemap|twlog'`
(five executables under core/tests/, linking only tw_core). twlog_test covers
ring wraparound accounting, 8-thread seq density, category interning against
address reuse, and the bounded non-blocking path.

Known debt: QtCore dependency (QString in twfraction.cpp tail); SAMPLE_NORM_*
are macros; DTOR_DEL macro survives for legacy call sites.
