#include "tw/plugins/twplugindescriptor.h"
#include "tw/plugins/twpluginsearchpaths.h"
#include "tw/plugins/twplugin.h"
#include "tw/core/twlog.h"

#include "twpluginscancache.h"

#include <QByteArray>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QThread>

#include <memory>
#include <utility>

#ifdef TW_HAVE_CLAP
// PRIVATE header of the CLAP backend. The TW_HAVE_CLAP define and the clap
// include directory are both PRIVATE to tw_plugins (see tw303a/CMakeLists.txt),
// so this #ifdef can only ever appear in a .cc inside this module — never in a
// public tw/plugins/*.h, where it would give other modules a different view of
// the same declarations.
#include "twclapmodule.h"
#endif

#ifdef TW_HAVE_VST3
// PRIVATE header of the VST3 backend, for the same reason as the CLAP one above.
#include "twvst3module.h"
#endif

#ifdef TW_HAVE_AU
// PRIVATE header of the AudioUnit backend (macOS), same PRIVATE-to-tw_plugins
// discipline as CLAP above. Declares only std::string/vector signatures, so no
// AudioToolbox type leaks into this translation unit.
#include "twaumodule.h"
#endif

namespace audio {

// Forward declare the PassThrough plugin factory.
std::unique_ptr<twPlugin> createPassThroughPlugin();
std::unique_ptr<twPlugin> createNativeInstrument();

// Static registry instance.
static twPluginRegistry gRegistry;

twPluginRegistry &pluginRegistry()
{
    return gRegistry;
}

namespace {

// Outcome of probing ONE module. Unavailable is not a plugin verdict: it says
// the out-of-process probe could not be run at all, so the caller falls back
// in-process rather than caching a bogus failure for every module on disk.
enum class ProbeOutcome { Ok, Failed, Timeout, Unavailable };

// In-process probe. Safe against a corrupt or foreign file (the loader refuses
// it and logs), NOT safe against a plugin that crashes while being created —
// which is exactly why the out-of-process path below is preferred.
ProbeOutcome probeInProcess( const twPluginModuleFile &m,
                             std::vector<twPluginDescriptor> &out )
{
    out.clear();
#ifdef TW_HAVE_CLAP
    if( m.format == "clap" ) {
        out = clapModuleDescriptors( m.path );
        return out.empty() ? ProbeOutcome::Failed : ProbeOutcome::Ok;
    }
#endif
#ifdef TW_HAVE_VST3
    if( m.format == "vst3" ) {
        out = vst3ModuleDescriptors( m.path );
        return out.empty() ? ProbeOutcome::Failed : ProbeOutcome::Ok;
    }
#endif
#ifdef TW_HAVE_AU
    if( m.format == "au" ) {
        out = auModuleDescriptors( m.path );
        return out.empty() ? ProbeOutcome::Failed : ProbeOutcome::Ok;
    }
#endif
    TW_LOGW( "plugins", "[scan] no in-process probe for format '%s' ('%s')",
             m.format.c_str(), m.path.c_str() );
    return ProbeOutcome::Failed;
}

std::vector<twPluginDescriptor> parseProbeJson( const QByteArray &raw,
                                                const std::string &modulePath )
{
    std::vector<twPluginDescriptor> out;

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson( raw, &err );
    if( err.error != QJsonParseError::NoError || !doc.isObject() ) {
        TW_LOGW( "plugins", "[scan] probe output for '%s' is not valid JSON (%s)",
                 modulePath.c_str(), err.errorString().toUtf8().constData() );
        return out;
    }
    for( const QJsonValue &pv : doc.object().value( "plugins" ).toArray() ) {
        if( !pv.isObject() ) continue;
        twPluginDescriptor d;
        if( descriptorFromJson( pv.toObject(), d ) ) {
            d.path = modulePath;      // the host knows where it probed
            out.push_back( std::move( d ) );
        }
    }
    return out;
}

// Out-of-process probe. A plugin that segfaults or hangs while being
// instantiated takes down the PROBE, and becomes a cached failed/timeout record
// — the app never sees it. This is proposal 08 §Decisions 2.
ProbeOutcome probeOutOfProcess( const twPluginModuleFile &m,
                                const std::string &probeExe, int timeoutMs,
                                std::vector<twPluginDescriptor> &out )
{
    out.clear();

    QProcess p;
    p.setProgram( QString::fromStdString( probeExe ) );
    p.setArguments( QStringList() << QString::fromStdString( m.path ) );
    // The probe's diagnostics go to stderr (TW_LOG*), its JSON to stdout, so
    // the two must not be merged.
    p.setProcessChannelMode( QProcess::SeparateChannels );
    p.start();

    if( !p.waitForStarted( 5000 ) ) {
        TW_LOGW( "plugins", "[scan] cannot start the plugin probe '%s' (%s) — "
                 "falling back to in-process scanning, WITHOUT crash isolation",
                 probeExe.c_str(), p.errorString().toUtf8().constData() );
        return ProbeOutcome::Unavailable;
    }

    if( !p.waitForFinished( timeoutMs ) ) {
        p.kill();
        p.waitForFinished( 2000 );
        TW_LOGW( "plugins", "[scan] probing '%s' timed out after %d ms; recording it "
                 "as a skipped module", m.path.c_str(), timeoutMs );
        return ProbeOutcome::Timeout;
    }

    if( p.exitStatus() != QProcess::NormalExit ) {
        TW_LOGW( "plugins", "[scan] the probe CRASHED on '%s'; recording it as a "
                 "skipped module", m.path.c_str() );
        return ProbeOutcome::Failed;
    }
    if( p.exitCode() != 0 ) {
        TW_LOGW( "plugins", "[scan] the probe refused '%s' (exit code %d)",
                 m.path.c_str(), p.exitCode() );
        return ProbeOutcome::Failed;
    }

    out = parseProbeJson( p.readAllStandardOutput(), m.path );
    return out.empty() ? ProbeOutcome::Failed : ProbeOutcome::Ok;
}

// Clears the flag on EVERY exit path, including an exception out of the scan.
struct ScanFlagGuard {
    std::atomic<bool> &flag;
    ~ScanFlagGuard() { flag.store( false, std::memory_order_release ); }
};

}  // namespace

twPluginRegistry::~twPluginRegistry()
{
    waitForScan();
}

// ---------------------------------------------------------------- config ----

void twPluginRegistry::setSearchPaths( std::vector<std::string> dirs )
{
    std::lock_guard<std::mutex> g( mutex_ );
    searchPaths_ = std::move( dirs );
}

std::vector<std::string> twPluginRegistry::searchPaths() const
{
    std::lock_guard<std::mutex> g( mutex_ );
    return searchPaths_;
}

void twPluginRegistry::setCachePath( std::string path )
{
    std::lock_guard<std::mutex> g( mutex_ );
    if( cachePath_ != path ) {
        cachePath_   = std::move( path );
        cacheLoaded_ = false;       // reload from the new location on next scan
        cache_.clear();
    }
}

void twPluginRegistry::setProbeExecutable( std::string path )
{
    std::lock_guard<std::mutex> g( mutex_ );
    probeExe_ = std::move( path );
}

void twPluginRegistry::setProbeTimeoutMs( int ms )
{
    std::lock_guard<std::mutex> g( mutex_ );
    probeTimeoutMs_ = ms > 0 ? ms : 1;
}

void twPluginRegistry::setScanProgress( twPluginScanProgressFn fn )
{
    std::lock_guard<std::mutex> g( mutex_ );
    progress_ = std::move( fn );
}

twPluginScanStats twPluginRegistry::scanStats() const
{
    std::lock_guard<std::mutex> g( mutex_ );
    return stats_;
}

// --------------------------------------------------------------- results ----

void twPluginRegistry::appendBuiltins_nolock()
{
    // The in-house test plugin is linked in, so it is never scanned for and
    // never cached. It stays FIRST in the list: plugins_test addresses it as
    // plugins()[0].
    twPluginDescriptor passThrough;
    passThrough.format = "tw";
    passThrough.uid    = "tw.passthrough";
    passThrough.path   = "";  // linked-in, not a separate module
    passThrough.name   = "PassThrough";
    passThrough.vendor = "Smaragd";
    passThrough.io     = { 2, 2 };
    passThrough.isInstrument = false;
    passThrough.nOutBuses = 1;
    passThrough.outBusChannels = { 2 };

    plugins_.push_back( passThrough );

    // The in-house 303 (proposal 36 D7). Linked in exactly like the pass-through
    // and for the same reason: the instrument path must be reachable in EVERY
    // build and every worktree, with no SDK, no submodule and nothing installed.
    // It appears in the browser like any other plugin, and it is the fallback
    // instrument every instrument gate can rely on being present.
    twPluginDescriptor native;
    native.format = "tw";
    native.uid    = "tw.native.303";
    native.path   = "";   // linked-in, not a separate module
    native.name   = "Smaragd 303";
    native.vendor = "Smaragd";
    native.io     = { 0, 1 };   // a generator: no audio in, one mono out
    native.isInstrument   = true;
    native.acceptsNotes   = true;
    native.emitsNotes     = false;
    native.eventPortsIn   = 1;
    native.eventPortsOut  = 0;
    native.nOutBuses      = 1;
    native.outBusChannels = { 1 };

    plugins_.push_back( native );
}

std::vector<twPluginDescriptor> twPluginRegistry::plugins() const
{
    std::lock_guard<std::mutex> g( mutex_ );
    if( plugins_.empty() ) {
        // Lazily seed the built-ins so a caller that never scanned still sees
        // them. NOT a lazy rescan: that would take the lock recursively and
        // would hit the disk from whatever thread happened to ask first.
        const_cast<twPluginRegistry *>( this )->appendBuiltins_nolock();
    }
    return plugins_;
}

bool twPluginRegistry::findByUid( const std::string &format, const std::string &uid,
                                  twPluginDescriptor &out ) const
{
    std::lock_guard<std::mutex> g( mutex_ );
    for( const twPluginDescriptor &d : plugins_ ) {
        if( d.uid == uid && ( format.empty() || d.format == format ) ) {
            out = d;
            return true;
        }
    }
    return false;
}

// --------------------------------------------------------------- scanning ---

bool twPluginRegistry::rescanAsync( bool force )
{
    std::lock_guard<std::mutex> g( threadMutex_ );
    if( scanThread_ ) {
        if( scanThread_->isRunning() ) return false;   // one scan at a time
        scanThread_->wait();
        delete scanThread_;
        scanThread_ = nullptr;
    }
    // Set BEFORE start(), so a caller that polls isScanning() immediately after
    // rescanAsync() cannot observe "already finished" before the thread runs.
    scanning_.store( true, std::memory_order_release );
    scanThread_ = QThread::create( [this, force]() { this->rescan( force ); } );
    scanThread_->setObjectName( "smaragd-plugin-scan" );
    scanThread_->start();
    return true;
}

void twPluginRegistry::waitForScan()
{
    std::lock_guard<std::mutex> g( threadMutex_ );
    if( !scanThread_ ) return;
    scanThread_->wait();
    delete scanThread_;
    scanThread_ = nullptr;
}

void twPluginRegistry::rescan( bool force )
{
    scanning_.store( true, std::memory_order_release );
    ScanFlagGuard flagGuard{ scanning_ };

    // Snapshot the configuration and the cache. Probing loads foreign code and
    // spawns processes, so the mutex is NOT held across it: plugins() and
    // scanStats() must stay answerable from the UI thread while a scan runs.
    std::vector<std::string>                    dirs;
    std::string                                 cachePath, probeExe;
    int                                         timeoutMs = 15000;
    std::map<std::string, twPluginModuleRecord> cache;
    twPluginScanProgressFn                      progress;
    {
        std::lock_guard<std::mutex> g( mutex_ );
        dirs      = searchPaths_;
        cachePath = cachePath_;
        probeExe  = probeExe_;
        timeoutMs = probeTimeoutMs_;
        if( !cacheLoaded_ ) {
            loadPluginScanCache( cachePath, kScannerVersion, cache_ );
            cacheLoaded_ = true;
        }
        cache    = cache_;
        progress = progress_;

        stats_         = twPluginScanStats();
        stats_.running = true;
    }

    // force is what makes a remembered failure recoverable: the user installed
    // a fixed build of the plugin, or the probe was broken. Everything is
    // re-probed, nothing is trusted.
    if( force ) cache.clear();

    std::vector<twPluginModuleFile> mods = enumeratePluginModules( dirs );

#ifdef TW_HAVE_AU
    // AudioUnits are discovered from the OS component registry, not by walking
    // the user's search paths (the registry is authoritative and covers system
    // units too). The resulting per-component module keys are merged into the
    // same list so they inherit the path-keyed cache, sticky-failure records and
    // out-of-process probe unchanged.
    //
    // SMARAGD_SCAN_AU=0 suppresses this: the headless scan gate asserts exact
    // module counts against a controlled fixture directory, which enumerating
    // every system AU would make non-deterministic. AU insert/instantiate does
    // NOT go through a scan, so disabling enumeration never affects the qxa
    // cases — only what the browser lists.
    if( qEnvironmentVariable( "SMARAGD_SCAN_AU", "1" ) != QLatin1String( "0" ) ) {
        const std::vector<twPluginModuleFile> au = enumerateAuModules();
        mods.insert( mods.end(), au.begin(), au.end() );
    }
#endif

    twPluginScanStats st;
    st.running      = true;
    st.modulesFound = (int) mods.size();

    // Rebuilt from scratch, so a module that disappeared from disk drops out of
    // the table instead of accumulating forever.
    std::map<std::string, twPluginModuleRecord> next;
    std::vector<twPluginDescriptor>            found;
    bool probeUsable = !probeExe.empty();

    for( const twPluginModuleFile &m : mods ) {
        st.currentPath = m.path;
        {
            std::lock_guard<std::mutex> g( mutex_ );
            stats_ = st;
        }
        if( progress ) progress( st );

        const auto it = cache.find( m.path );
        if( it != cache.end()
            && it->second.sizeBytes      == m.sizeBytes
            && it->second.mtimeMs        == m.mtimeMs
            && it->second.scannerVersion == kScannerVersion ) {
            next[m.path] = it->second;
            if( it->second.status == twPluginModuleStatus::Ok ) {
                ++st.modulesCached;
                for( const twPluginDescriptor &d : it->second.plugins )
                    found.push_back( d );
            } else {
                // The whole point of remembering failures: this module is not
                // touched again until it changes on disk or force is passed.
                ++st.modulesSkipped;
                TW_LOGD( "plugins", "[scan] skipping '%s' (remembered as %s)",
                         m.path.c_str(),
                         it->second.status == twPluginModuleStatus::Timeout
                             ? "timeout" : "failed" );
            }
            continue;
        }

        twPluginModuleRecord rec;
        rec.path           = m.path;
        rec.sizeBytes      = m.sizeBytes;
        rec.mtimeMs        = m.mtimeMs;
        rec.scannerVersion = kScannerVersion;

        ProbeOutcome po = ProbeOutcome::Unavailable;
        if( probeUsable ) {
            po = probeOutOfProcess( m, probeExe, timeoutMs, rec.plugins );
            if( po == ProbeOutcome::Unavailable ) {
                // A probe that will not start will not start for the next
                // module either; stop paying 5 s per module for it.
                probeUsable = false;
            }
        }
        if( po == ProbeOutcome::Unavailable )
            po = probeInProcess( m, rec.plugins );

        ++st.modulesProbed;
        switch( po ) {
        case ProbeOutcome::Ok:
            rec.status = twPluginModuleStatus::Ok;
            for( const twPluginDescriptor &d : rec.plugins ) found.push_back( d );
            TW_LOGI( "plugins", "[scan] '%s': %u plugin(s)", m.path.c_str(),
                     (unsigned) rec.plugins.size() );
            break;
        case ProbeOutcome::Timeout:
            rec.status = twPluginModuleStatus::Timeout;
            rec.plugins.clear();
            ++st.modulesFailed;
            break;
        default:
            rec.status = twPluginModuleStatus::Failed;
            rec.plugins.clear();
            ++st.modulesFailed;
            break;
        }
        next[m.path] = std::move( rec );
    }

    st.currentPath.clear();
    st.running = false;

    savePluginScanCache( cachePath, kScannerVersion, next );

    {
        std::lock_guard<std::mutex> g( mutex_ );
        cache_ = std::move( next );
        plugins_.clear();
        appendBuiltins_nolock();
        for( const twPluginDescriptor &d : found ) plugins_.push_back( d );
        st.pluginsFound = (int) plugins_.size();
        stats_          = st;
    }

    TW_LOGI( "plugins", "[scan] done: %d module(s) found, %d probed, %d cached, "
             "%d skipped, %d failed -> %d plugin(s)",
             st.modulesFound, st.modulesProbed, st.modulesCached,
             st.modulesSkipped, st.modulesFailed, st.pluginsFound );

    if( progress ) progress( st );
}

// ---------------------------------------------------------- instantiation ---

std::unique_ptr<twPlugin> twPluginRegistry::instantiate( const twPluginDescriptor &desc )
{
    if( desc.uid == "tw.passthrough" ) {
        return createPassThroughPlugin();
    }
    if( desc.uid == "tw.native.303" ) {
        return createNativeInstrument();
    }

    if( desc.format == "clap" ) {
#ifdef TW_HAVE_CLAP
        // Symbol-referenced discovery (CONTRACT invariant 1): the registry names
        // the backend factory directly, so nothing depends on static-init
        // self-registration surviving static-library linking.
        return createClapPlugin( desc.path, desc.uid );
#else
        TW_LOGE( "plugins", "[registry] cannot instantiate CLAP plugin '%s': this build "
                 "has no CLAP support (the third_party/clap submodule was missing at "
                 "configure time)", desc.uid.c_str() );
        return nullptr;
#endif
    }

    if( desc.format == "vst3" ) {
#ifdef TW_HAVE_VST3
        return createVst3Plugin( desc.path, desc.uid );
#else
        TW_LOGE( "plugins", "[registry] cannot instantiate VST3 plugin '%s': this build "
                 "has no VST3 support (the third_party/vst3_pluginterfaces submodule was "
                 "missing at configure time)", desc.uid.c_str() );
        return nullptr;
#endif
    }

    if( desc.format == "au" ) {
#ifdef TW_HAVE_AU
        return createAuPlugin( desc.path, desc.uid );
#else
        TW_LOGE( "plugins", "[registry] cannot instantiate AudioUnit plugin '%s': this "
                 "build has no AU support (AU hosting is macOS-only)", desc.uid.c_str() );
        return nullptr;
#endif
    }

    TW_LOGW( "plugins", "[registry] no backend for plugin format '%s' (uid '%s')",
             desc.format.c_str(), desc.uid.c_str() );
    return nullptr;
}

}  // namespace audio
