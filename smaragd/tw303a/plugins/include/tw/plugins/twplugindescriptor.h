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

    // --- proposal 37 P2: what the scanner learned about EVENTS ---------------
    //
    // Derived from a live instance's capabilities() at scan time, exactly like
    // `io` is derived from its ioLayout(): none of it is a descriptor field in
    // any format. They exist so the browser can offer a Kind filter and
    // "Add Instrument" without instantiating every plugin in the list, and so a
    // project that references a plugin this machine does not have still knows
    // the SHAPE it will get (plugins/CONTRACT.md invariant 17).
    bool          acceptsNotes  = false;
    bool          emitsNotes    = false;
    std::uint16_t eventPortsIn  = 0;
    std::uint16_t eventPortsOut = 0;

    // Audio OUTPUT buses. nOutBuses is >= 1 for anything that makes sound; bus
    // 0 is the main bus and its channel count equals io.audioOutputs. The rest
    // are aux outs (proposal 37 §5.4, built in P9).
    std::uint16_t              nOutBuses = 0;
    std::vector<std::uint16_t> outBusChannels;
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
    // 1 -> 2 (proposal 37 P2): the descriptor gained acceptsNotes, emitsNotes,
    // eventPortsIn/Out, nOutBuses and outBusChannels. A v1 record cannot supply
    // them, so every record — including the remembered failures — is
    // invalidated once and re-probed.
    static constexpr int kScannerVersion = 2;

    twPluginRegistry() = default;
    ~twPluginRegistry();

    twPluginRegistry( const twPluginRegistry & )            = delete;
    twPluginRegistry &operator=( const twPluginRegistry & ) = delete;

    // --- configuration (app-supplied; the registry itself stays dumb) -------

    // Directories searched for module files, recursively. Defaults come from
    // twPluginSearchPaths::defaults(); the app persists the user's edited list.
    void setSearchPaths( std::vector<std::string> dirs );
    std::vector<std::string> searchPaths() const;

    // <configDir>/plugincache.json. Empty disables persistence (the scan then
    // still works, it is just cold on every start).
    void setCachePath( std::string path );

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
    bool rescanAsync( bool force = false );

    bool isScanning() const { return scanning_.load( std::memory_order_acquire ); }
    void waitForScan();

    // Ask a running scan to stop at the next module boundary, then join it.
    // This is what the app's ORDERLY TEARDOWN calls (SApplication's destructor
    // and main.cpp's smaragdOrderlyShutdown) so the scan thread is gone BEFORE
    // static destruction begins: a --test-case run leaves through std::exit(),
    // where no stack object is destroyed, and a scan thread still alive at that
    // point used to log into an already-destroyed sink (see plan/STATE.md
    // 2026-08-16). Whatever the aborted scan had already probed is still
    // written to the cache, so successive runs converge instead of restarting
    // cold every time. Safe to call when no scan is running, and safe to call
    // twice.
    void stopScan();

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
    std::atomic<bool>               stopRequested_{ false };  // set by stopScan()
    mutable std::mutex              threadMutex_;   // guards scanThread_ only
    QThread                        *scanThread_ = nullptr;
};

// Factory to get or create the singleton registry.
twPluginRegistry &pluginRegistry();

}  // namespace audio

#endif
