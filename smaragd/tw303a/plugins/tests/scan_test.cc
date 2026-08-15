// plugins_scan_test — the scanner/cache gate for proposal 08 M2.
//
// What is actually asserted here, in the order a user meets it:
//   1. a cold scan finds a real CLAP module and records a corrupt one as failed
//   2. the second scan probes NOTHING (cache hit on path+size+mtime+version)
//   3. changing a module's mtime re-probes exactly that module
//   4. a failed record is STICKY — the corrupt module is never probed again,
//      which is what stops one bad plugin from costing a probe every launch
//   5. rescan( force ) clears the remembered failures
//   6. the cache survives a fresh registry instance (it is on disk, keyed by
//      scannerVersion) and its failed record is still a failed record
//   7. the out-of-process probe reaches the same verdicts, and a corrupt module
//      does not take the caller down
//   8. rescanAsync() + waitForScan() runs a scan off the calling thread
//
// The "real module" is the in-repo fixture twtestclap.clap, so nothing has to be
// installed. The "bad module" is a file of garbage with a .clap name: it is the
// cheapest thing that makes both the in-process loader and the probe refuse it.
// A module that HANGS (the Timeout record) has no cheap fixture and is covered
// by the code path's shared handling plus the manual pass, not by this test.

#include "tw/plugins/twplugindescriptor.h"
#include "tw/plugins/twpluginsearchpaths.h"

#include "twpluginscancache.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace audio {

namespace {

int gFailures = 0;

bool check( bool ok, const std::string &what )
{
    if( ok ) {
        std::cout << "  ok   " << what << std::endl;
    } else {
        std::cerr << "  FAIL " << what << std::endl;
        ++gFailures;
    }
    return ok;
}

std::string statsLine( const twPluginScanStats &s )
{
    return "found=" + std::to_string( s.modulesFound )
         + " probed=" + std::to_string( s.modulesProbed )
         + " cached=" + std::to_string( s.modulesCached )
         + " skipped=" + std::to_string( s.modulesSkipped )
         + " failed=" + std::to_string( s.modulesFailed )
         + " plugins=" + std::to_string( s.pluginsFound );
}

bool hasUid( const std::vector<twPluginDescriptor> &v, const std::string &uid )
{
    for( const twPluginDescriptor &d : v )
        if( d.uid == uid ) return true;
    return false;
}

// Give a file an explicitly different modification time. Doing it by hand
// instead of rewriting the file makes the assertion about the CACHE KEY, not
// about filesystem timestamp granularity.
bool setMtime( const QString &path, qint64 msSinceEpoch )
{
    QFile f( path );
    if( !f.open( QIODevice::ReadWrite ) ) return false;
    const bool ok = f.setFileTime( QDateTime::fromMSecsSinceEpoch( msSinceEpoch ),
                                   QFileDevice::FileModificationTime );
    f.close();
    return ok;
}

bool writeGarbage( const QString &path )
{
    QFile f( path );
    if( !f.open( QIODevice::WriteOnly | QIODevice::Truncate ) ) return false;
    // Not a PE/ELF/Mach-O image by any reading: the loader must refuse it.
    const QByteArray junk( 4096, '\x7f' );
    f.write( junk );
    f.close();
    return true;
}

const char *kGainUid = "tw.test.clap.gain";

}  // namespace
}  // namespace audio

int main( int argc, char **argv )
{
    // QProcess (the out-of-process probe) needs a QCoreApplication instance.
    QCoreApplication app( argc, argv );

    using namespace audio;

    std::cout << "=== plugin search-path defaults ===" << std::endl;
    {
        const std::vector<std::string> clap = twPluginSearchPaths::defaults( "clap" );
        check( !clap.empty(), "defaults(\"clap\") offers at least one location" );
        for( const std::string &d : clap )
            std::cout << "       " << d << std::endl;
        check( twPluginSearchPaths::defaults( "nonesuch" ).empty(),
               "defaults() refuses an unknown format instead of guessing" );
    }

    // ---- a scratch tree with one real module and one corrupt one ------------
    const QString root = QDir( QDir::tempPath() ).filePath(
        QString( "smaragd_scan_test_%1" ).arg( QCoreApplication::applicationPid() ) );
    QDir( root ).removeRecursively();
    const QString modDir   = QDir( root ).filePath( "plugins" );
    const QString cachePath = QDir( root ).filePath( "plugincache.json" );
    if( !QDir().mkpath( modDir ) ) {
        std::cerr << "  FAIL cannot create " << modDir.toStdString() << std::endl;
        return 1;
    }
    const QString goodMod   = QDir( modDir ).filePath( "testgain.clap" );
    const QString brokenMod = QDir( modDir ).filePath( "broken.clap" );

#ifdef TW_TESTCLAP_PATH
    const bool haveFixture = QFile::copy( QString( TW_TESTCLAP_PATH ), goodMod );
    audio::check( haveFixture, "copied the twtestclap.clap fixture into the scan tree" );
#else
    const bool haveFixture = false;
    std::cout << "  note this build has no CLAP support; only the corrupt-module "
                 "and cache paths are exercised" << std::endl;
#endif
    audio::check( audio::writeGarbage( brokenMod ), "wrote a corrupt broken.clap" );

    const int expectFound = haveFixture ? 2 : 1;
    const int expectOk    = haveFixture ? 1 : 0;

    // ---- 1..5: in-process probing, which is the pure cache-logic case -------
    std::cout << "=== cache miss / hit / invalidate / stickiness (in-process) ==="
              << std::endl;
    {
        twPluginRegistry reg;
        reg.setSearchPaths( { modDir.toStdString() } );
        reg.setCachePath( cachePath.toStdString() );

        reg.rescan( false );
        twPluginScanStats s = reg.scanStats();
        std::cout << "       cold: " << audio::statsLine( s ) << std::endl;
        audio::check( s.modulesFound == expectFound, "cold scan sees both module files" );
        audio::check( s.modulesProbed == expectFound, "cold scan probes every module" );
        audio::check( s.modulesCached == 0 && s.modulesSkipped == 0,
                      "cold scan has nothing cached and nothing skipped" );
        audio::check( s.modulesFailed == 1, "the corrupt module is recorded as failed" );
        if( haveFixture )
            audio::check( audio::hasUid( reg.plugins(), audio::kGainUid ),
                          "the fixture's plugin is in the registry" );
        audio::check( audio::hasUid( reg.plugins(), "tw.passthrough" ),
                      "the built-in plugin is still listed after a scan" );

        reg.rescan( false );
        s = reg.scanStats();
        std::cout << "       warm: " << audio::statsLine( s ) << std::endl;
        audio::check( s.modulesProbed == 0, "the second scan probes NOTHING" );
        audio::check( s.modulesCached == expectOk, "the good module is served from cache" );
        audio::check( s.modulesSkipped == 1,
                      "the failed record is sticky: the corrupt module is skipped" );
        if( haveFixture )
            audio::check( audio::hasUid( reg.plugins(), audio::kGainUid ),
                          "a cache-served scan still yields the plugin" );

        if( haveFixture ) {
            audio::check( audio::setMtime( goodMod,
                              QDateTime::currentMSecsSinceEpoch() - 3600 * 1000 ),
                          "changed the good module's mtime" );
            reg.rescan( false );
            s = reg.scanStats();
            std::cout << "       touched: " << audio::statsLine( s ) << std::endl;
            audio::check( s.modulesProbed == 1,
                          "an mtime change re-probes exactly that module" );
            audio::check( s.modulesCached == 0, "...and nothing is served from cache" );
            audio::check( s.modulesSkipped == 1,
                          "...while the corrupt module stays skipped" );
            audio::check( audio::hasUid( reg.plugins(), audio::kGainUid ),
                          "the re-probed plugin is back in the registry" );
        }

        reg.rescan( true );
        s = reg.scanStats();
        std::cout << "       forced: " << audio::statsLine( s ) << std::endl;
        audio::check( s.modulesProbed == expectFound, "force re-probes everything" );
        audio::check( s.modulesSkipped == 0, "force clears the remembered failures" );
        audio::check( s.modulesFailed == 1, "the corrupt module fails again" );

        twPluginDescriptor found;
        if( haveFixture ) {
            audio::check( reg.findByUid( "clap", audio::kGainUid, found ),
                          "findByUid resolves the scanned plugin" );
            audio::check( found.path == goodMod.toStdString(),
                          "...to the module file it was scanned from" );
            audio::check( found.io.audioInputs == 2 && found.io.audioOutputs == 2,
                          "...with the module's real channel counts" );
        }
        audio::check( !reg.findByUid( "clap", "nope.not.here", found ),
                      "findByUid refuses an unknown uid" );
    }

    // ---- 6: the cache is on disk and a fresh registry trusts it -------------
    std::cout << "=== the cache survives a new registry instance ===" << std::endl;
    {
        audio::check( QFileInfo::exists( cachePath ), "plugincache.json was written" );

        std::map<std::string, twPluginModuleRecord> table;
        audio::check( loadPluginScanCache( cachePath.toStdString(),
                                           twPluginRegistry::kScannerVersion, table ),
                      "the cache file parses" );
        auto brokenRec = table.find( brokenMod.toStdString() );
        audio::check( brokenRec != table.end(), "the corrupt module has a record" );
        if( brokenRec != table.end() )
            audio::check( brokenRec->second.status != twPluginModuleStatus::Ok,
                          "...and it is not an 'ok' record" );

        twPluginRegistry reg2;
        reg2.setSearchPaths( { modDir.toStdString() } );
        reg2.setCachePath( cachePath.toStdString() );
        reg2.rescan( false );
        const twPluginScanStats s = reg2.scanStats();
        std::cout << "       reloaded: " << audio::statsLine( s ) << std::endl;
        audio::check( s.modulesProbed == 0,
                      "a fresh registry probes nothing: the cache came off disk" );
        audio::check( s.modulesSkipped == 1,
                      "the failed record is still remembered across instances" );
        if( haveFixture )
            audio::check( audio::hasUid( reg2.plugins(), audio::kGainUid ),
                          "and the plugin is available without loading the module" );

        // A cache written by a different scanner version must be discarded
        // wholesale, not partially trusted.
        std::map<std::string, twPluginModuleRecord> other;
        audio::check( !loadPluginScanCache( cachePath.toStdString(),
                                            twPluginRegistry::kScannerVersion + 1,
                                            other ),
                      "a cache from another scannerVersion is refused" );
        audio::check( other.empty(), "...and yields nothing partially trusted" );
    }

    // ---- 6b: AC7 — a version-1 cache is invalidated and rescanned ONCE -------
    //
    // kScannerVersion went 1 -> 2 in proposal 36 P2, because the descriptor
    // gained fields a v1 record cannot supply. The concrete promise is: an
    // existing plugincache.json from the previous build is thrown away, every
    // module is re-probed exactly once, and the SECOND scan is a cache hit again
    // — i.e. the invalidation is a one-off, not a permanent cold start.
    std::cout << "=== a scanner-v1 cache is invalidated once (proposal 36 P2) ===" << std::endl;
    {
        const QString v1Cache = QDir( root ).filePath( "plugincache_v1.json" );

        // Write a well-formed cache that CLAIMS version 1 and holds a record for
        // every module — the shape the previous build left behind.
        {
            twPluginRegistry warm;
            warm.setSearchPaths( { modDir.toStdString() } );
            warm.setCachePath( v1Cache.toStdString() );
            warm.rescan( true );

            QFile f( v1Cache );
            if( audio::check( f.open( QIODevice::ReadOnly ),
                              "the freshly written cache is readable" ) ) {
                const QByteArray raw = f.readAll();
                f.close();
                QJsonDocument doc = QJsonDocument::fromJson( raw );
                QJsonObject   root2 = doc.object();
                audio::check( root2.value( "scannerVersion" ).toInt( -1 ) == 2,
                              "this build writes scannerVersion 2" );
                root2["scannerVersion"] = 1;   // pretend it came from the old build
                if( f.open( QIODevice::WriteOnly | QIODevice::Truncate ) ) {
                    f.write( QJsonDocument( root2 ).toJson( QJsonDocument::Compact ) );
                    f.close();
                }
            }
        }

        twPluginRegistry reg;
        reg.setSearchPaths( { modDir.toStdString() } );
        reg.setCachePath( v1Cache.toStdString() );

        reg.rescan( false );   // NOT forced: the version alone must invalidate it
        const twPluginScanStats s = reg.scanStats();
        std::cout << "       after v1: " << audio::statsLine( s ) << std::endl;
        audio::check( s.modulesProbed == expectFound,
                      "a v1 cache is discarded and every module is re-probed" );
        audio::check( s.modulesCached == 0, "...nothing was served from it" );

        reg.rescan( false );
        const twPluginScanStats s2 = reg.scanStats();
        audio::check( s2.modulesProbed == 0,
                      "the rescan happened ONCE: the next scan hits the new cache" );
    }

    // ---- 7: the same verdicts through the out-of-process probe --------------
#ifdef TW_PLUGINPROBE_PATH
    std::cout << "=== out-of-process probe (" << TW_PLUGINPROBE_PATH << ") ===" << std::endl;
    {
        const QString probeCache = QDir( root ).filePath( "plugincache_probe.json" );
        twPluginRegistry reg;
        reg.setSearchPaths( { modDir.toStdString() } );
        reg.setCachePath( probeCache.toStdString() );
        reg.setProbeExecutable( QString( TW_PLUGINPROBE_PATH ).toStdString() );
        reg.setProbeTimeoutMs( 30000 );

        reg.rescan( true );
        const twPluginScanStats s = reg.scanStats();
        std::cout << "       probed: " << audio::statsLine( s ) << std::endl;
        audio::check( s.modulesProbed == expectFound,
                      "the probe was run for every module" );
        audio::check( s.modulesFailed == 1,
                      "the corrupt module fails in the probe, and we are still alive" );
        if( haveFixture ) {
            audio::check( audio::hasUid( reg.plugins(), audio::kGainUid ),
                          "the probe's JSON round-trips the plugin descriptor" );
            twPluginDescriptor d;
            reg.findByUid( "clap", audio::kGainUid, d );
            audio::check( d.io.audioInputs == 2 && d.io.audioOutputs == 2,
                          "...including the real channel counts" );
            audio::check( d.name == "Smaragd Test Gain",
                          "...and the plugin name" );

            // AC7: the probe's JSON schema carries the scanner-v2 fields. The
            // gain effect has none of them set, so the discriminating case is
            // the SINE instrument from the same module.
            audio::check( !d.acceptsNotes && d.nOutBuses == 1 &&
                              d.outBusChannels.size() == 1 && d.outBusChannels[0] == 2,
                          "...and the effect's (empty) event fields + its one bus" );

            twPluginDescriptor sine;
            if( audio::check( reg.findByUid( "clap", "tw.test.clap.sine", sine ),
                              "the probe also reports the sine instrument" ) ) {
                audio::check( sine.isInstrument && sine.acceptsNotes &&
                                  sine.eventPortsIn == 1 && !sine.emitsNotes,
                              "...with its INSTRUMENT feature and its note port" );
                audio::check( sine.nOutBuses == 2 && sine.outBusChannels.size() == 2 &&
                                  sine.outBusChannels[0] == 2 &&
                                  sine.outBusChannels[1] == 1,
                              "...and both output buses (stereo main + mono aux)" );
            }
            twPluginDescriptor arp;
            if( audio::check( reg.findByUid( "clap", "tw.test.clap.arp", arp ),
                              "the probe also reports the arpeggiator" ) ) {
                audio::check( arp.acceptsNotes && arp.emitsNotes &&
                                  arp.eventPortsIn == 1 && arp.eventPortsOut == 1,
                              "...with note ports in AND out" );
                audio::check( arp.nOutBuses == 0,
                              "...and no audio output bus at all" );
            }
        }

        reg.rescan( false );
        const twPluginScanStats s2 = reg.scanStats();
        audio::check( s2.modulesProbed == 0,
                      "no process is spawned when the cache is warm" );
        audio::check( s2.modulesSkipped == 1,
                      "the corrupt module is not re-probed on the next scan" );
    }
#else
    std::cout << "  note no probe executable in this build; the out-of-process "
                 "half is skipped" << std::endl;
#endif

    // ---- 8: the scan really does run off the calling thread -----------------
    std::cout << "=== rescanAsync ===" << std::endl;
    {
        twPluginRegistry reg;
        reg.setSearchPaths( { modDir.toStdString() } );
        reg.setCachePath( cachePath.toStdString() );
        audio::check( reg.rescanAsync( false ), "rescanAsync started a scan" );
        // plugins() must remain answerable while the worker runs — that is the
        // reason it returns by value instead of by const reference.
        (void) reg.plugins();
        (void) reg.scanStats();
        reg.waitForScan();
        audio::check( !reg.isScanning(), "the scan thread finished and was joined" );
        audio::check( reg.scanStats().modulesFound == expectFound,
                      "the async scan produced the same module count" );
    }

    // ---- 9: .vst3 discovery (proposal 08 M6) --------------------------------
    //
    // Its own tree and its own registry, deliberately: the counts above are
    // asserted exactly, and threading a second format through them would couple
    // two unrelated properties. What matters here is only that the scanner now
    // REPORTS .vst3 — before M6 formatForFile() returned nullptr for it on
    // purpose, so that an unloadable module could not be cached as a permanent
    // failure.
#ifdef TW_TESTVST3_PATH
    std::cout << "=== .vst3 discovery ===" << std::endl;
    {
        const QString v3Dir   = QDir( root ).filePath( "vst3" );
        const QString v3Cache = QDir( root ).filePath( "plugincache_vst3.json" );
        QDir().mkpath( v3Dir );
        const QString v3Mod = QDir( v3Dir ).filePath( "testgain.vst3" );
        if( audio::check( QFile::copy( QString( TW_TESTVST3_PATH ), v3Mod ),
                          "copied the twtestvst3.vst3 fixture into a scan tree" ) ) {
            twPluginRegistry reg;
            reg.setSearchPaths( { v3Dir.toStdString() } );
            reg.setCachePath( v3Cache.toStdString() );

            reg.rescan( false );
            const twPluginScanStats s = reg.scanStats();
            std::cout << "       vst3: " << audio::statsLine( s ) << std::endl;
            audio::check( s.modulesFound == 1, "the scanner reports a .vst3 module file" );
            audio::check( s.modulesProbed == 1, "...and probes it" );
            audio::check( s.modulesFailed == 0, "...successfully" );

            // TWO vst3 descriptors since proposal 36 P2 (the gain effect and
            // the SPLIT sine instrument); the instrument's controller class is
            // not an audio-effect class and must not appear.
            const std::vector<twPluginDescriptor> all = reg.plugins();
            const twPluginDescriptor *v3   = nullptr;
            const twPluginDescriptor *sine = nullptr;
            int nVst3 = 0;
            for( const twPluginDescriptor &d : all ) {
                if( d.format != "vst3" ) continue;
                ++nVst3;
                if( d.name == "TW Test VST3 Gain" ) v3 = &d;
                if( d.name == "TW Test VST3 Sine" ) sine = &d;
            }
            audio::check( nVst3 == 2, "both audio-effect classes reached the registry" );
            if( audio::check( v3 != nullptr, "a format=\"vst3\" descriptor reached the registry" ) ) {
                audio::check( v3->uid.size() == 32, "its uid is a 32-hex-digit class id" );
                audio::check( v3->name == "TW Test VST3 Gain", "its name is the class name" );
                audio::check( v3->io.audioInputs == 2 && v3->io.audioOutputs == 2,
                              "its I/O came from a live instance" );

                twPluginDescriptor byUid;
                audio::check( reg.findByUid( "vst3", v3->uid, byUid ),
                              "findByUid resolves it, as a saved project would" );
            }
            // AC7: the SCANNER (version 2) records the event/bus fields, and
            // they survive the cache round trip below.
            if( audio::check( sine != nullptr, "the SPLIT instrument was scanned" ) ) {
                audio::check( sine->isInstrument, "...as an instrument" );
                audio::check( sine->acceptsNotes && sine->eventPortsIn == 1,
                              "...with its kEvent input bus recorded" );
                audio::check( !sine->emitsNotes && sine->eventPortsOut == 0,
                              "...and no event output" );
                audio::check( sine->nOutBuses == 1 && sine->outBusChannels.size() == 1 &&
                                  sine->outBusChannels[0] == 2,
                              "...and one stereo output bus" );
            }

            reg.rescan( false );
            audio::check( reg.scanStats().modulesProbed == 0,
                          "the second scan serves the .vst3 from cache" );
            audio::check( reg.scanStats().modulesCached == 1,
                          "...and counts it as cached" );

            // The version-2 fields must survive the JSON round trip, or a
            // cached instrument would come back looking like an effect.
            const twPluginDescriptor *cachedSine = nullptr;
            for( const twPluginDescriptor &d : reg.plugins() )
                if( d.name == "TW Test VST3 Sine" ) cachedSine = &d;
            if( audio::check( cachedSine != nullptr, "the cached instrument is back" ) ) {
                audio::check( cachedSine->isInstrument && cachedSine->acceptsNotes &&
                                  cachedSine->eventPortsIn == 1 &&
                                  cachedSine->nOutBuses == 1,
                              "the scanner-v2 fields round-trip through plugincache.json" );
            }
        }
    }
#else
    std::cout << "  note this build has no VST3 support; .vst3 discovery is skipped"
              << std::endl;
#endif

    QDir( root ).removeRecursively();

    if( audio::gFailures ) {
        std::cerr << audio::gFailures << " check(s) FAILED" << std::endl;
        return 1;
    }
    std::cout << "all plugin scan checks passed" << std::endl;
    return 0;
}
