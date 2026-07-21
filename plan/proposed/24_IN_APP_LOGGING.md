# Proposal 24 — In-application logging: one sink, a dockable log view

Status: PROPOSED (2026-07-21) — re-verified against `9e6e2c3` (2026-07-21)

## 1. Motivation

Diagnostic output is scattered across three unrelated channels that all end at
the console and nowhere else:

| Channel | Sites | Where it goes today |
|---|---|---|
| `qDebug/qInfo/qWarning/qCritical` | 144 | Qt's default handler — on Windows/MinGW the **Windows debug channel, not stderr** (see §1.1) |
| `syslog(LOG_*, …)` via `tw/core/twsyslog.h` | 36 | `vfprintf(stderr)` on Windows; the **real** syslog on POSIX |
| raw `fprintf(stderr, …)` | 104 (84 engine, 20 app) | stderr |

Four concrete costs:

1. **The Windows build links the CONSOLE subsystem purely to make logging
   visible** (`SMARAGD_WIN_CONSOLE`, `main/CMakeLists.txt:305`) — a console
   window pops for every user, and turning it off makes the app mute.
2. **Half the engine's messages are invisible on Linux/macOS**: the `syslog()`
   path lands in the system journal, not in front of the developer.
3. **Nothing survives a crash**, and a user cannot hand back a log.
4. **Records carry no severity, no module, no thread.** In a codebase whose hard
   bugs are worker/RT-thread races (the capture-reval crash cluster, the split
   repaint UAF, `revalAddRef` atomicity), "which thread said this, and when
   relative to that other line" is the question the log cannot answer.

`plan/STATE.md` records this pain **twice, months apart**, which is the clearest
signal that it is structural rather than incidental:

- `STATE.md:5870` (drag-clip-edge work) — *"`qWarning()` output is invisible in
  this Windows/MinGW build, so action-level diagnostics do not reach the test
  log — the failures that matter must be expressed as assertions, not
  warnings."*
- `STATE.md:2642` ("Diagnostic note (for next time)") — *"On Windows/MinGW,
  `qWarning()`/`qDebug()` output does **not** reach the bash-redirected `stderr`
  logfile (it routes to the Windows debug channel), so DnD probes via `qWarning`
  were invisible. Engine logs show up because `twsyslog.h` uses
  `fprintf(stderr,…)+fflush`. Use that same idiom (not `qWarning`) when adding
  console diagnostics that must land in a redirected log."*

Diagnostics that cannot be seen force every observation to become an assertion,
and the standing workaround — "prefer `fprintf` over `qWarning`" — is precisely
what grew the 104 raw call sites this proposal has to sweep back up.

### 1.1 A note on why the Qt bridge is not merely cosmetic

The second entry pins down the mechanism: Qt's **default** handler on
Windows/MinGW routes to `OutputDebugString`, so `qWarning`/`qDebug` never reach
a redirected `stderr` — which is why the engine's `syslog()` output appears in
captured logs and the app's Qt output does not. `qInstallMessageHandler`
*replaces* that default handler outright. Every Qt message therefore leaves
through `TwLog`'s own `fwrite(stderr)` tee, on the same file descriptor as
everything else, correctly interleaved. Installing the handler (§3) fixes a
twice-recorded, years-old defect as a side effect, and retires the "use
`fprintf`, not `qWarning`" advice that STATE.md currently gives.

**This has a direct consequence for verification (§8.2): the console output
after the change is a strict superset of the output before it.** 144 Qt call
sites that were previously invisible on redirected stderr will start appearing
there. A "nothing changed" diff is the *wrong* success criterion.

**Goal.** One structured sink that every channel feeds; console output stays the
default in Debug and becomes an option in Release; and a dockable log window that
stays responsive at hundreds of thousands of lines.

---

## 2. The sink — `tw/core/twlog.h` + `core/src/twlog.cc`

`tw/core` is the right home. Its CMake comment already claims the role — *"core:
types, format, fraction, conversion, **logging**/exceptions"*
(`tw303a/CMakeLists.txt:27`) — it depends on nothing but QtCore, and
`check_layering.py` allows `core` from **every** app and engine module
(`tools/check_layering.py:102`, `_ENG_BASE = {'core', 'graph'}`). Reachability
from everywhere is the whole requirement, and only `core` has it.

The sink is plain C++: no `QObject`, no signals, no Qt types on the producer
path. This is not stylistic — it is the standing rule that engine and worker
threads must never touch Qt (`docs/contracts/THREADING.md`; the adoption
deadlock recorded in STATE.md).

```cpp
namespace tw {

enum class LogLevel { Error = 0, Warn, Info, Debug, Trace };

struct LogRecord {
    uint64_t    seq;       // monotonic, never reused — the reader's cursor
    int64_t     tMonoUs;   // steady_clock µs since sink start
    LogLevel    level;
    uint16_t    catId;     // interned category ("devices", "ui.timeline", …)
    uint32_t    threadId;  // interned thread slot
    std::string text;      // exactly one line
};

class TwLog {
public:
    static TwLog &instance();

    // --- configuration (main() only) ---
    void setCapacity ( size_t records );                 // default 200000
    void setConsole  ( bool on );
    void setMinLevel ( LogLevel );                       // cheap global gate
    void setFileSink ( const std::string &dir, size_t maxBytes, int keep );
    void shutdown    ();                                 // flush + join writer

    // --- producer side (any thread) ---
    void logf  ( LogLevel, const char *cat, const char *file, int line,
                 const char *fmt, ... ) TW_PRINTF_FMT(6, 7);
    void vlogf ( LogLevel, const char *cat, const char *file, int line,
                 const char *fmt, va_list );
    void logStr( LogLevel, const char *cat, const char *file, int line,
                 const std::string &msg );               // Qt bridge
    static void markNonBlocking();                       // RT threads call once

    // --- reader side (GUI thread + file writer) ---
    uint64_t firstSeq() const;      // oldest still resident
    uint64_t nextSeq () const;      // one past newest
    size_t   snapshot( uint64_t from, uint64_t to, std::vector<LogRecord> &out ) const;
    uint64_t droppedCount() const;
    static const char *categoryName( uint16_t );
    static const char *threadName  ( uint32_t );
};
}  // namespace tw

#define TW_LOGE(cat, ...) ::tw::TwLog::instance().logf(::tw::LogLevel::Error, cat, __FILE__, __LINE__, __VA_ARGS__)
#define TW_LOGW(cat, ...) …Warn…
#define TW_LOGI(cat, ...) …Info…
#define TW_LOGD(cat, ...) …Debug…
```

### 2.1 Invariants

1. **The ring never allocates in steady state.** `std::vector<LogRecord>` of
   fixed capacity, slot = `seq % capacity`. Overwriting an old record `assign`s
   into its existing `std::string` — the capacity is reused, never freed. Every
   slot is `reserve(256)`d at construction.
2. **`seq` is monotonic and never reused.** It is the reader's cursor and the
   only thing that makes "what have I not yet seen" answerable without holding a
   lock across the read.
3. **One record is one line.** Messages containing `\n` are split at emit time.
   The table view is one row per record; nothing downstream has to re-wrap.
4. **`markNonBlocking()` threads never block and never allocate.** They format
   into a `thread_local char[256]` and take the slot with `try_lock`; on failure
   they bump `dropped_` and return. Long messages from such a thread are
   truncated with a `…` marker rather than growing the slot string.
5. **Producers hold one mutex for O(1) work.** Contention at realistic rates
   (hundreds/s) is nil; the file writer and the UI both read through
   `snapshot()`, so there is no second queue to keep coherent.

### 2.2 RT-thread path

The realtime callback marks itself once, next to the existing
`twRtThreadGuard::markRtThread()` call in `tw303a/playback/src/twspeaker.cc:173`.
`twRtThreadGuard` (`tw303a/graph/include/tw/graph/tw_freeze_context.h:47`) gains
a one-line delegation to `TwLog::markNonBlocking()` so the two markers can never
drift apart. This matters because the guard's own violation report is itself a
log line emitted *from* the RT thread.

### 2.3 Compile-time format checking

`TW_PRINTF_FMT(n, m)` expands to `__attribute__((format(printf, n, m)))` on
GCC/Clang (empty elsewhere). This is what makes the `fprintf` sweep of §4 safe:
every rewritten call site keeps printf semantics and is validated by the
compiler, so a mis-transcribed argument list is a build error, not a runtime
corruption.

### 2.4 Console tee and file sink

**Console.** A single preformatted `fwrite` to stderr under the same lock, only
when `console_` is set. Format:
`HH:MM:SS.mmm [W] devices/audio  WASAPIBackend: GetMixFormat failed: 0x88890004`.

**File.** A dedicated writer thread with **no second queue** — it holds its own
`seq` cursor and drains via the same `snapshot()` the UI uses, every 250 ms. If
it falls behind the ring it writes a `--- N records dropped ---` marker rather
than silently losing the gap. Rotates `smaragd.log` → `smaragd.log.1…N` at
`maxBytes` (default 8 MB, keep 3), in the QSettings config directory
(`QStandardPaths::AppConfigLocation` — the same root `SSettings` already uses,
`main/shell/src/ssettings.cpp:12`).

---

## 3. Bridges — 179 of 283 sites change nothing

**`tw/core/twsyslog.h`.** Rewrite the shim body so `syslog()` forwards to
`TwLog::vlogf` with category `"syslog"` and `LOG_*` mapped onto `LogLevel`. Do
this on **all** platforms — drop the `#include <syslog.h>` passthrough — so the
36 ALSA/CoreAudio/dsp sites become visible in-app on Linux and macOS instead of
vanishing into the journal. The `LOG_*` constants stay defined; no call site
changes.

**Qt.** `qInstallMessageHandler` as the very first statement of `main()`
(`main/shell/src/main.cpp:15`), before `SApplication` construction — the headless
`-platform offscreen` argv rewrite above it must stay first in *source* order but
emits nothing, so the handler still precedes any Qt message. Map
`QtDebugMsg→Debug`, `QtInfoMsg→Info`, `QtWarningMsg→Warn`,
`QtCriticalMsg/QtFatalMsg→Error`; category from `QMessageLogContext::category`
(`"default"` for bare `qDebug()`), file/line from the context. `QtFatalMsg` must
still abort — flush the file sink first.

---

## 4. The `fprintf` sweep — 104 sites

Mechanical rewrite of `fprintf(stderr, "…\n", …)` → `TW_LOGE/W/I/D(cat, "…", …)`,
dropping the trailing `\n`. Category from the owning module directory:

| Path | Category |
|---|---|
| `tw303a/<mod>/src/…` | `"<mod>"` — `devices`, `graph`, `mix`, `schedule`, `playback`, `render`, `sources`, `dsp`, `plugins`, `record` |
| `main/objects/<x>/…`, `main/model`, `main/persistence` | `"<x>"`, `"model"`, `"persistence"` |
| `main/timeline`, `main/shell`, `main/testkit`, `main/servicesui` | `"ui.timeline"`, `"ui.shell"`, `"ui.testkit"`, `"ui.services"` |

Level by content: `failed`/`error`/`invalid` → `TW_LOGE`;
`warn`/`recover`/`xrun`/`already` → `TW_LOGW`; `opened`/`using`/`config` →
`TW_LOGI`; everything else `TW_LOGD`.

Heaviest files: `tw303a/devices/src/wasapi_backend.cc` (~20),
`main/shell/src/smainwindow.cpp` (26 mixed channels),
`main/timeline/src/sstdmixerview.cpp` (21 mixed),
`tw303a/playback/src/audio_engine.cc` (15).

### 4.1 Explicitly out of scope

- **`std::cout` in `main/shell/src/main.cpp:68–172`** — the TAP `PASS`/`FAIL`,
  `# Actions applied:`, `# Failures:`, `# Artifacts:` output that the qxa harness
  parses. Touching it breaks the test suite; it is program output, not logging.
- Any `fprintf(stdout, …)` / bare `printf` that is program output rather than
  diagnostics — notably in `tools/` and `tw303a/*/tests/`.

### 4.2 Regression guard

New `tools/check_logging.py`: fails on any `fprintf(std(err|out)` or bare
`printf(` outside an allowlist (the `main.cpp` TAP block, `tools/`, `*/tests/`,
and `twlog.cc`'s own console tee). Added beside `check_layering.py` in the
pre-commit line of `CLAUDE.md` and `docs/ARCHITECTURE.md`. Without this the
sweep decays within a month.

---

## 5. Console policy

`SMARAGD_WIN_CONSOLE` (Windows subsystem selection) stays as it is. Runtime
console control is **orthogonal** to it and layered — last wins:

1. **Compile default** — `SMARAGD_LOG_CONSOLE_DEFAULT`, `ON` for
   `CMAKE_BUILD_TYPE=Debug`, `OFF` otherwise. *(This is the "default in debug,
   by option in release" requirement.)*
2. **Persisted option** — `SOpt::LogConsole`, `SOpt::LogLevel`,
   `SOpt::LogCapacity`, `SOpt::LogToFile`: new keys in
   `main/servicesui/include/app/servicesui/soptions.h` with defaults in
   `SOpt::def()`, surfaced on a new **Log** tab in `SOptionsDialog`.
3. **Env** — `SMARAGD_LOG_CONSOLE=0|1`,
   `SMARAGD_LOG_LEVEL=error|warn|info|debug|trace`, matching the existing
   `SMARAGD_REVAL_WORKERS` convention.
4. **CLI** — `--log-console` / `--no-log-console` / `--log-level=<lvl>`, added to
   the `QCommandLineParser` already in `main.cpp:56`.

Two-phase init in `main()`: install the handler + ring + console immediately, so
the earliest startup messages are captured; call `setFileSink()` once the config
path is known.

---

## 6. The dock — `main/servicesui/`

`servicesui` is the declared home for dialogs over engine services, and
`shell → servicesui` is already an allowed edge (`tools/check_layering.py:92–95`).
New files:

- `servicesui/include/app/servicesui/slogmodel.h` + `src/slogmodel.cpp`
- `servicesui/include/app/servicesui/slogview.h`  + `src/slogview.cpp`

Registered in `main/CMakeLists.txt` next to the existing servicesui block
(lines 205–215).

### 6.1 `SLogModel : QAbstractTableModel` — why not a proxy

Columns: **Time | Level | Category | Thread | Message**.

The model holds its **own filtered materialization** (`std::vector<Row>`, message
already a `QString`), *not* a `QSortFilterProxyModel`. This is the central
performance decision. A proxy over 200 000 rows rebuilds its full source mapping
on every filter change and re-evaluates on every batch insert — precisely the
"massively impacts the application" failure mode the requirement rules out.
Filtering here is a predicate applied once, at drain time.

- A 100 ms `QTimer`, **running only while the dock is visible**, calls
  `TwLog::snapshot(cursor_, TwLog::nextSeq(), tmp)`, filters `tmp`, and issues a
  single `beginInsertRows()` for the whole surviving batch.
- Per-tick drain is capped (~20 000 records) so a burst cannot stall the GUI
  thread; the remainder arrives on the next tick.
- Eviction: at most `viewCap_` rows (default = ring capacity), removed as one
  `beginRemoveRows(0, k)`.
- Filter change → `beginResetModel()` + one full re-scan of the resident ring.
  O(N) once, for an explicit user action — acceptable.
- `data()` does no formatting and no allocation: every field is precomputed at
  insert time. Level → colour via `Qt::ForegroundRole`.

### 6.2 `SLogView : QWidget`

`QTableView` configured for constant-time layout — this is the load-bearing part:

- `verticalHeader()->setSectionResizeMode(QHeaderView::Fixed)` +
  `setDefaultSectionSize(rowH)` → scrollbar geometry becomes arithmetic and
  painting is O(visible rows), independent of row count.
- Message column `Stretch`; the other four sized **once** from a sample row, then
  set `Fixed`. `ResizeToContents` on a growing model is O(N) *per insert* and
  would dominate everything else.
- `setWordWrap(false)`, `setTextElideMode(Qt::ElideRight)`, `setShowGrid(false)`,
  `setSelectionBehavior(SelectRows)`, `ScrollPerPixel` on both axes.

Toolbar: level combo · category multi-select (`QToolButton` + checkable menu,
populated from the interned ids seen so far) · substring filter (debounced
200 ms) · Pause/Resume · Clear · Copy selection · Save as… · Open log folder ·
an "**N dropped**" indicator.

Autoscroll follows the tail **only when the vertical scrollbar is already at its
maximum**, so scrolling up to read pins the view — standard console behaviour.
Pause stops the drain timer but not the sink; resuming replays from the ring.

### 6.3 `SMainWindow` wiring — `main/shell/src/smainwindow.cpp`

- Create `qDockLog_` next to the existing `qDockExternFileList_` block (line 855):
  `new QDockWidget(tr("Log"), this)`, `setObjectName("dock_log")`,
  `addDockWidget(Qt::BottomDockWidgetArea, …)`, hidden on first run. The
  objectName is what lets the **existing** `saveState`/`restoreState` persistence
  (`SSettings::windowState`, `smainwindow.cpp:482`) restore its visibility and
  placement for free — no new settings key.
- New top-level **`&View`** menu (there is none today) built next to the other
  menus at `smainwindow.cpp:841`, holding `qDockLog_->toggleViewAction()` — Qt
  supplies the checkable action — bound to **Ctrl+Shift+L**. Move
  `qDockExternFileList_->toggleViewAction()` in there as well.
- Connect `QDockWidget::visibilityChanged` → `SLogView::setLive(bool)` so the
  drain timer is idle whenever the dock is closed. Cost when closed: zero.
- `TwLog::shutdown()` at the end of `main()`, after `app.exec()`.

---

## 7. Files

**New**

- `tw303a/core/include/tw/core/twlog.h`, `tw303a/core/src/twlog.cc`
- `tw303a/core/tests/test_twlog.cpp` — the `core/tests/test_<name>.cpp` naming
  the four existing core tests use
- `main/servicesui/{include/app/servicesui,src}/slogmodel.{h,cpp}`
- `main/servicesui/{include/app/servicesui,src}/slogview.{h,cpp}`
- `tools/check_logging.py`

**Modified**

- `tw303a/CMakeLists.txt` — twlog sources in the `core` module (~line 27) **and**
  `tw_module_test(twlog_test core/tests/test_twlog.cpp tw_core)` beside the other
  four core tests (~line 359);
  `main/CMakeLists.txt` (servicesui sources ~205; `SMARAGD_LOG_CONSOLE_DEFAULT`
  beside `SMARAGD_WIN_CONSOLE` ~305)
- `tw303a/core/include/tw/core/twsyslog.h` (shim → `TwLog`, all platforms)
- `tw303a/graph/include/tw/graph/tw_freeze_context.h`,
  `tw303a/playback/src/twspeaker.cc` (`markNonBlocking` beside `markRtThread`)
- `main/shell/src/main.cpp` (message handler, CLI flags, two-phase init, shutdown)
- `main/shell/src/smainwindow.cpp` + `include/app/shell/smainwindow.h`
  (View menu, log dock)
- `main/servicesui/{include/app/servicesui/soptions.h, src/soptions.cpp,
  src/soptionsdialog.cpp}` (Log tab + keys)
- The ~104 sweep sites — pattern in §4; representative:
  `tw303a/devices/src/wasapi_backend.cc`, `tw303a/playback/src/audio_engine.cc`,
  `main/shell/src/smainwindow.cpp`, `main/timeline/src/sstdmixerview.cpp`
- `tw303a/core/CONTRACT.md`, `main/servicesui/CONTRACT.md`,
  `docs/ARCHITECTURE.md`, `CLAUDE.md` (pre-commit line)

---

## 8. Verification

1. **Build both configurations** — Debug (`./build.sh`) and
   `-DCMAKE_BUILD_TYPE=Release`; confirm the console default flips and that
   `SMARAGD_WIN_CONSOLE=OFF` + `--log-console` behaves sensibly (no console
   window, log still reaches the dock and file).
2. **Console superset** — run `smaragd --log-console --log-level=debug` through a
   startup + play + render sequence, redirect stderr to a file, and diff against
   a pre-change run. The correct criterion is **superset, not equality** (§1.1):
   every pre-change line must still be present (modulo the new
   `HH:MM:SS.mmm [W] cat ` prefix), *plus* the Qt messages that previously went
   to the Windows debug channel. Assert specifically that nothing is **lost** —
   additions are the point of the change.
3. **Regression suite — the one that matters most**, because the sweep touches
   testkit: run the qxa suite **from `smaragd/tests/cases/`** (fixtures are
   CWD-relative; running from `smaragd/` yields bogus failures). All cases green,
   TAP stdout byte-identical to before.
4. **Unit test** `tw303a/core/tests/test_twlog.cpp`:
   - fill past capacity → `firstSeq()` advances, `snapshot()` returns exactly the
     resident window, `droppedCount()` exact;
   - 8 threads × 100 000 records → every `seq` in `[firstSeq, nextSeq)` appears
     exactly once, no torn strings;
   - a `markNonBlocking()` thread under contention never blocks (assert bounded
     wall time) and never allocates for ≤255-byte messages.
5. **Determinism gate** — byte-level `cmp` of a rendered WAV before/after (16-bit
   PCM, not float32) must match: the sweep must not perturb the audio path.
   Sweep `SMARAGD_REVAL_WORKERS` over {1,4,8,16} with
   `tests/repeat_test.sh … takes_group_broadcast 50`.
6. **UI scale test** — a `logstress` testkit action emitting 500 000 records.
   The assertions must be **numeric, not visual**: `SScreenshotAction` grabs the
   root window (`screen->grabWindow(0)`, `sscreenshotaction.cpp:49`), i.e. the
   whole desktop, so a screenshot cannot verify the dock's state — it is an
   eyeball aid only, and `test-output/` is untracked as of `9e6e2c3`.
   Instrument behind `SMARAGD_LOG_DEBUG=1` and assert, via a new
   `assert-log-drain-under <ms>` testkit action: (a) the per-tick drain stays
   under ~5 ms at steady state, (b) the worst single tick during the 500 k burst
   stays bounded (the per-tick cap of §6.1 is what guarantees this — it is the
   assertion that would actually catch a regression to an uncapped drain),
   (c) a full-ring substring filter completes well under a second, (d) `rowCount`
   settles at `min(500000, capacity)` with `droppedCount()` accounting for the
   remainder. Manual pass afterwards for scroll feel (jump to top, jump to
   bottom, drag the scrollbar).
7. **File sink** — kill the app mid-run and confirm
   `%APPDATA%/Smaragd/smaragd.log` holds records up to the kill; confirm rotation
   at 8 MB and that exactly 3 backups are kept.
8. **Layering** — `python tools/check_layering.py` and the new
   `python tools/check_logging.py` both clean.

---

## 9. Follow-ups deliberately not in scope

- **Third-party output** (libsndfile, ALSA, Qt platform plugins) still bypasses
  the sink. Capturing it needs an fd-2 pipe tee with a reader thread; records
  would carry no level or category. Worth doing only if that noise turns out to
  matter — the sweep plus `check_logging.py` covers all of *our* code.
- **Per-category level thresholds** (mute `schedule` at Debug while keeping
  `devices` at Trace). The record shape already carries `catId`, so this is a
  filter-side addition, not a re-design.
- **Log-driven test assertions** (`assert-log-contains` as a qxa action). Now
  that diagnostics are structured and addressable this becomes cheap, and it
  directly answers the STATE.md note that closed the drag-clip-edge work.
