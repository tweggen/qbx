# tw/core — CONTRACT

Purpose: the bottom of the dependency graph — value types and pure utilities
every other module builds on. Nothing here knows about components, pages,
files, or the app.

Public headers: twtypes.h (sample_t/offset_t/length_t/idx_t, preview_t,
SAMPLE_NORM_*), twformat.h (twFormat + capability domains), twfraction.h
(exact rational arithmetic for positions), twconvert.h (twConvertFrames —
format conversion, NO rate change), generation_promise.h
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
6. TwLog IS IMMORTAL. TwLog::instance() returns a heap instance that is created
   once and NEVER destroyed, so a record emitted from any thread at any point
   in process teardown can never lock a destroyed mutex. A function-local
   `static TwLog inst` could not hold that property: it is constructed at the
   FIRST log call (inside main) and is therefore destroyed EARLY -- before
   namespace-scope statics that own threads, one of which is the plugin
   registry, whose scan thread logs while it is being joined. That combination
   hung the process after PASS (plan/STATE.md 2026-08-16). The consequence is
   that shutdown() -- flush + join the file writer -- is an EXPLICIT call from
   the app's orderly teardown (main.cpp's smaragdOrderlyShutdown, on every path
   out of main including the --test-case std::exit), never a destructor.
   Logging after shutdown() is safe and reaches the ring; it just does not
   reach the file. Asserted by twlog_test (late records from a fresh thread,
   plus an atexit handler registered before the sink exists).

7. **String→double parsing in `parseFractionOrDouble` is LOCALE-INDEPENDENT**
   (found 2026-08-21: every clip time-COMPRESSED with `resize-clip
   stretch="0.5"` rendered as digital silence, on Linux only). `std::stod`/
   `strtod` read the C library's GLOBAL locale — the one `std::setlocale()`
   mutates — not the separate C++ `std::locale` mechanism, and Qt's platform
   integration calls `setlocale(LC_ALL, "")` while constructing `QApplication`
   (confirmed by probe, including under `QT_QPA_PLATFORM=offscreen`) whenever
   the environment's `LC_NUMERIC` names a comma-decimal locale — e.g.
   `de_DE.UTF-8`, an ordinary developer-machine setting on Linux, and never
   seen on Windows because the Win32 CRT does not adopt `LC_NUMERIC` from the
   environment on its own. Every `.qxp`/`.qxa` attribute this parser reads is
   written with `.` as the decimal point, so under a comma locale
   `std::stod("0.5")` silently parsed only the leading `"0"` and returned
   `0.0` — collapsing to `Fraction(0)`, which every caller downstream (e.g.
   `twGrainSource`'s ctor) then clamps away from zero as if no stretch had
   been requested at all. `parseDoubleInvariant()` (a `std::istringstream`
   imbued with `std::locale::classic()` — a mechanism `setlocale()` cannot
   touch) is the ONLY correct way to parse a decimal literal anywhere in this
   codebase; never add a bare `std::stod`/`std::strtod` call on a value that
   came from a project file, a script attribute, or any other portable
   spelling.

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
address reuse, the bounded non-blocking path, and the immortality of the sink
(invariant 6).

Known debt: QtCore dependency (QString in twfraction.cpp tail); SAMPLE_NORM_*
are macros; DTOR_DEL macro survives for legacy call sites.
