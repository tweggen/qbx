#ifndef _TWLOG_H_
#define _TWLOG_H_

// The process-wide log sink (proposal 24).
//
// Every diagnostic channel in the project funnels here: the twsyslog.h shim,
// Qt's qDebug/qWarning family (via qInstallMessageHandler, installed in main),
// and the TW_LOG* macros below. Records land in a fixed-capacity ring; from
// there the console tee, the rotating file writer, and the in-app log dock all
// read the SAME records, so the three views can never disagree.
//
// This header lives in tw/core because core is the only module every other
// module — engine and app alike — is allowed to include (tools/check_layering.py,
// _ENG_BASE). It is deliberately plain C++: no QObject, no signals, no Qt types
// on the producer path. Engine and worker threads must never touch Qt (see
// docs/contracts/THREADING.md — a Qt thread adoption here deadlocks teardown).

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// printf-style argument checking for the logf/TW_LOG* family. This is what makes
// the fprintf->TW_LOG sweep safe: a mis-transcribed argument list is a build
// error rather than a runtime corruption.
#if defined(__GNUC__) || defined(__clang__)
#  define TW_PRINTF_FMT(fmtIdx, argIdx) __attribute__((format(printf, fmtIdx, argIdx)))
#else
#  define TW_PRINTF_FMT(fmtIdx, argIdx)
#endif

namespace tw {

enum class LogLevel {
    Error = 0,
    Warn  = 1,
    Info  = 2,
    Debug = 3,
    Trace = 4
};

// Longest message a non-blocking (realtime) producer may emit. Longer messages
// from such a thread are truncated; ordinary threads are unbounded.
inline constexpr size_t TW_LOG_RT_MAX = 256;

struct LogRecord {
    uint64_t    seq      = 0;  // monotonic, never reused — the reader's cursor
    int64_t     tMonoUs  = 0;  // microseconds since sink start (steady_clock)
    LogLevel    level    = LogLevel::Info;
    uint16_t    catId    = 0;  // interned category ("devices", "ui.timeline", …)
    uint32_t    threadId = 0;  // interned thread slot, in first-seen order
    std::string text;          // exactly one line, no trailing newline
};

class TwLog
{
public:
    static TwLog &instance();

    // ---- configuration: main() only, before threads start logging ----------

    // Ring capacity in records. Re-sizing discards what is already buffered.
    void setCapacity( size_t records );
    size_t capacity() const;

    // Tee every accepted record to stderr as it is emitted.
    void setConsole( bool on );
    bool console() const;

    // Records above this level are dropped at the call site, as cheaply as we
    // can manage (one relaxed atomic load, no lock).
    void setMinLevel( LogLevel lvl );
    LogLevel minLevel() const;

    // Start the rotating-file writer in `dir`. Rotates smaragd.log ->
    // smaragd.log.1 … .N once it passes maxBytes, keeping `keep` backups.
    // Called once the config directory is known; the ring and console are live
    // from the very first record, so nothing before this call is lost.
    void setFileSink( const std::string &dir, size_t maxBytes, int keep );

    // Flush and join the writer thread. Call at the end of main().
    void shutdown();

    // ---- producer side: any thread ----------------------------------------

    void logf( LogLevel lvl, const char *cat, const char *file, int line,
               const char *fmt, ... ) TW_PRINTF_FMT(6, 7);
    void vlogf( LogLevel lvl, const char *cat, const char *file, int line,
                const char *fmt, va_list ap );
    // Pre-formatted variant — the Qt bridge, which already holds a QString.
    void logStr( LogLevel lvl, const char *cat, const char *file, int line,
                 const std::string &msg );

    // Mark THIS thread as one that must never block: it will format into a
    // thread-local buffer and take the ring slot with try_lock, counting a drop
    // rather than waiting. The realtime audio callback calls this once, next to
    // twRtThreadGuard::markRtThread().
    static void markNonBlocking();
    static bool nonBlocking();

    // Name the calling thread for the log's Thread column ("audio-rt", "gui",
    // "reval-3", …). Falls back to a numeric slot if never called.
    static void nameThread( const char *name );

    // ---- reader side: GUI drain + file writer ------------------------------
    //
    // Readers hold a cursor and ask for [from, to). Records evicted since the
    // last call are silently skipped — compare the returned range against
    // firstSeq() to detect it, or use droppedCount().

    uint64_t firstSeq() const;   // oldest still resident
    uint64_t nextSeq() const;    // one past the newest ever emitted

    // Copy the resident subrange of [from, to) into `out` (cleared first).
    // Returns the number of records copied.
    size_t snapshot( uint64_t from, uint64_t to, std::vector<LogRecord> &out ) const;

    // Records that never reached the ring: emitted while a non-blocking
    // producer could not take the lock.
    uint64_t droppedCount() const;

    // Interned name lookup. Both are stable for the process lifetime and safe
    // to call from any thread; ids are small and dense.
    static const char *categoryName( uint16_t catId );
    static uint16_t    categoryCount();
    static const char *threadName( uint32_t threadId );
    static uint32_t    threadCount();

    static const char *levelName( LogLevel lvl );   // "ERR" / "WRN" / …

    // Format one record the way the console tee and the file writer do:
    //   12:34:56.789 [WRN] devices  WASAPIBackend: …
    static std::string formatLine( const LogRecord &rec );

private:
    TwLog();
    ~TwLog();
    TwLog( const TwLog & ) = delete;
    TwLog &operator=( const TwLog & ) = delete;

    struct Impl;
    Impl *d_;
};

}  // namespace tw

// The call-site macros. `cat` is a short module string ("devices", "schedule",
// "ui.timeline") — it is interned once per distinct pointer-or-string, so a
// literal costs nothing per call.
#define TW_LOGE( cat, ... ) \
    ::tw::TwLog::instance().logf( ::tw::LogLevel::Error, (cat), __FILE__, __LINE__, __VA_ARGS__ )
#define TW_LOGW( cat, ... ) \
    ::tw::TwLog::instance().logf( ::tw::LogLevel::Warn,  (cat), __FILE__, __LINE__, __VA_ARGS__ )
#define TW_LOGI( cat, ... ) \
    ::tw::TwLog::instance().logf( ::tw::LogLevel::Info,  (cat), __FILE__, __LINE__, __VA_ARGS__ )
#define TW_LOGD( cat, ... ) \
    ::tw::TwLog::instance().logf( ::tw::LogLevel::Debug, (cat), __FILE__, __LINE__, __VA_ARGS__ )
#define TW_LOGT( cat, ... ) \
    ::tw::TwLog::instance().logf( ::tw::LogLevel::Trace, (cat), __FILE__, __LINE__, __VA_ARGS__ )

#endif
