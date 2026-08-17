#ifndef _TWPLUGINDESCRIPTOR_H_
#define _TWPLUGINDESCRIPTOR_H_

#include "tw/plugins/twplugin.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class QThread;

namespace audio {

struct twPluginDescriptor {
    std::string   format;             // "tw" (in-house) | "clap" | "vst3" | "au" | "lv2"
    std::string   uid;                // format-stable unique id
    std::string   path;               // module file (empty for linked-in plugins)
    std::string   name, vendor;
    twPluginIoLayout io;
    bool          isInstrument = false;
};

// What the scanner learned about ONE module file (proposal 08 M2).
//
// Failed and Timeout are REMEMBERED, not retried: a plugin that crashes or
// hangs the probe would otherwise cost a probe on every launch, and a user with
// one bad module would pay for it forever. rescan( force = true ) is the only
// thing that clears them.
enum class twPluginModuleStatus { Ok = 0, Failed, Timeout };

// One cache record. The identity is path + sizeBytes + mtimeMs +
// scannerVersion: any difference re-probes, an exact match is served from the
// cache without loading the module at all.
struct twPluginModuleRecord {
    std::string          path;
    std::uint64_t        sizeBytes      = 0;
    std::int64_t         mtimeMs        = 0;
    int                  scannerVersion = 0;
    twPluginModuleStatus status         = twPluginModuleStatus::Ok;
    std::vector<twPluginDescriptor> plugins;   // empty unless status == Ok
};

// Progress/summary of a scan. Reported through the twPluginScanProgressFn AND
// readable at any time via twPluginRegistry::scanStats().
struct twPluginScanStats {
    int  modulesFound   = 0;   // module files the search paths yielded
    int  modulesProbed  = 0;   // actually loaded/probed this run
    int  modulesCached  = 0;   // served from the cache (status Ok)
    int  modulesSkipped = 0;   // remembered failed/timeout, not re-probed
    int  modulesFailed  = 0;   // probed this run and failed/timed out
    int  pluginsFound   = 0;   // descriptors now in the registry (incl. built-ins)
    bool running        = false;
    std::string currentPath;   // module being probed (empty when idle)
};

// Called during a scan. IMPORTANT: it runs on the SCAN THREAD, not the UI
// thread. An app-side implementation must only touch atomics — surfacing this
// to Qt widgets is done by polling from a main-thread timer, because a Qt
// signal emitted from a worker thread makes Qt adopt it and its TLS cleanup
// deadlocks the teardown join (a recorded, hard-won invariant of this repo).
using twPluginScanProgressFn = std::function<void( const twPluginScanStats & )>;

class twPluginRegistry {
public:
    // Bumped whenever the scanner's output for an unchanged file could differ
    // (new descriptor field, changed I/O derivation, ...). It is part of the
    // cache key, so bumping it invalidates every record — including the
    // remembered failures.
    static constexpr int kScannerVersion = 1;

    twPluginRegistry() = default;
    ~twPluginRegistry();

    twPluginRegistry( const twPluginRegistry & )            = delete;
    twPluginRegistry &operator=( const twPluginRegistry & ) = delete;

    // --- configuration (app-supplied; the registry itself stays dumb) -------

    // Directories searched for module files, recursively. Defaults come from
    // twPluginSearchPaths::defaults(); the app persists the user's edited list.
    void setSearchPaths( std::vector<std::string> dirs );
    std::vector<std::string> searchPaths() const;

    // <configDir>/<cacheFileName()>. Empty disables persistence (the scan then
    // still works, it is just cold on every start).
    void setCachePath( std::string path );

    // The file name the app must give setCachePath(): SCOPED BY SCANNER
    // VERSION, "plugincache.v<kScannerVersion>.json".
    //
    // kScannerVersion is a source constant while the cache is one file per
    // USER, so two builds at different versions used to share it and reject
    // each other's records on EVERY launch -- a permanently cold cache and a
    // full re-probe of every installed plugin in every process, which is what
    // left a scan running long enough to still be in flight at exit. Records
    // from a foreign version were already discarded wholesale on load, so a
    // per-version file loses exactly nothing: each build just keeps its own
    // warm table instead of trampling the other's.
    static std::string cacheFileName();

    // The out-of-process probe executable (smaragd_pluginprobe). Empty, or a
    // path that cannot be started, falls back to probing IN-PROCESS — which is
    // fine for a corrupt file (the loader refuses it) but offers no isolation
    // against a plugin that crashes while being instantiated.
    void setProbeExecutable( std::string path );
    void setProbeTimeoutMs( int ms );

    void setScanProgress( twPluginScanProgressFn fn );

    // --- scanning ----------------------------------------------------------

    // Synchronous scan on the CALLING thread. Never call it from the UI thread
    // in the app (it loads code and spawns processes) — that is what
    // rescanAsync() is for. Direct calls are how the headless tests drive it.
    void rescan( bool force = false );

    // Start a scan on a worker QThread. Returns false if one is already
    // running. A QThread (not a raw std::thread) deliberately: the scan uses
    // QProcess and QJson, and a std::thread adopted by Qt deadlocks teardown.
    //
    // It CLEARS any pending cancellation before starting: a cancel belongs to
    // the scan that was running when it was asked for, never to the next one.
    bool rescanAsync( bool force = false );

    // Ask a running scan to stop at the next module boundary (and to kill the
    // probe process it is currently waiting on). Shutdown is the only caller
    // that matters: without it, teardown could do nothing but wait out a full
    // re-probe of every plugin installed on the machine.
    //
    // Cheap and idempotent; safe from any thread. A scan that is already
    // finished ignores it, and the flag is cleared by the next rescanAsync()
    // or rescan().
    //
    // A cancelled scan does NOT write the cache file. The table it has is
    // partial, and the one caller is a process on its way out — writing from
    // there is the kind of teardown-time I/O this whole fix exists to avoid.
    void cancelScan();
    bool scanCancelled() const { return scanCancel_.load( std::memory_order_acquire ); }

    bool isScanning() const { return scanning_.load( std::memory_order_acquire ); }

    // Join the scan worker. timeoutMs < 0 waits forever (what the headless
    // tests want, where a scan that never ends IS the failure); a shutdown path
    // must pass a bound, and must pair it with cancelScan() first or the bound
    // is just a slower way to wait out a full re-probe.
    //
    // Returns true when the thread was joined and disposed of. False means the
    // wait expired and the worker is STILL RUNNING: the QThread is deliberately
    // neither deleted nor forgotten (deleting a running QThread is undefined,
    // and dropping the pointer would let a later join silently succeed against
    // nothing), so a subsequent waitForScan() gets another chance at the same
    // thread. A false return is worth a LOUD log — from the caller, not from
    // here, because the last-resort caller is ~twPluginRegistry running under
    // static destruction, where the log sink may already be gone.
    bool waitForScan( int timeoutMs = -1 );

    twPluginScanStats scanStats() const;

    // --- results -----------------------------------------------------------

    // BY VALUE, deliberately: a worker thread may replace the list at any
    // moment, so handing out a reference into it is a data race.
    std::vector<twPluginDescriptor> plugins() const;

    // Resolve a (format, uid) pair — how a saved project re-finds its plugin.
    bool findByUid( const std::string &format, const std::string &uid,
                    twPluginDescriptor &out ) const;

    // Instantiate a plugin from its descriptor.
    std::unique_ptr<twPlugin> instantiate( const twPluginDescriptor & );

private:
    void appendBuiltins_nolock();

    // The scan body. clearCancel is false for the call rescanAsync() makes:
    // it has ALREADY cleared the flag, before start(), so clearing it again
    // from the worker would silently swallow a cancel that arrived in between
    // — which is exactly the teardown race this exists to survive.
    void rescanImpl( bool force, bool clearCancel );

    mutable std::mutex              mutex_;         // guards everything below
    std::vector<twPluginDescriptor> plugins_;
    std::vector<std::string>        searchPaths_;
    std::string                     cachePath_;
    std::string                     probeExe_;
    int                             probeTimeoutMs_ = 15000;
    std::map<std::string, twPluginModuleRecord> cache_;
    bool                            cacheLoaded_ = false;
    twPluginScanStats               stats_;
    twPluginScanProgressFn          progress_;

    std::atomic<bool>               scanning_{ false };
    std::atomic<bool>               scanCancel_{ false };
    mutable std::mutex              threadMutex_;   // guards scanThread_ only
    QThread                        *scanThread_ = nullptr;
};

// Factory to get or create the singleton registry.
twPluginRegistry &pluginRegistry();

}  // namespace audio

#endif
