// asio_probe — de-risking spike for proposal 35 Phase 1 (ASIO backend).
//
// This is the GATE the proposal names: before one line of asio_device.cc is
// written, prove that a binary built by Qt's bundled MinGW g++ can drive an
// MSVC-built ASIO driver end to end. The same role vst3_probe played for
// proposal 08 M6 — and the same ABI bet: x64 Windows has ONE calling
// convention, so calling through a foreign C++ vtable works as long as both
// sides agree on the vtable layout and the POD structs.
//
// What it has to answer:
//
//   1. Can we enumerate installed drivers WITHOUT the SDK's host sources?
//      (asio_driver_list.cc scans HKLM\SOFTWARE\ASIO with plain advapi32 —
//      compiling the SDK's host/pc/asiolist.cpp on MinGW is what we are
//      trying to avoid.)
//   2. Does CoCreateInstance(clsid, ..., clsid, ...) — ASIO's idiosyncratic
//      "the CLSID doubles as the IID" convention — hand back an object whose
//      IASIO vtable we can call? init/getDriverName landing correctly is the
//      first proof; garbage in the name would mean a vtable-layout mismatch.
//   3. Do the PODs crossing the boundary agree on layout? ASIOChannelInfo,
//      ASIOBufferInfo and ASIOTime are plain structs the DRIVER reads and
//      writes; an implausible channel count or sample type is the tell,
//      exactly like BusInfo was for VST3.
//   4. Does the driver calling back INTO US work? bufferSwitch /
//      bufferSwitchTimeInfo / asioMessage are C function pointers invoked
//      from a driver-owned thread — the reverse direction of the ABI, which
//      a query-only test would never exercise.
//   5. Does a real start() → bufferSwitch → stop() cycle deliver audio at
//      the negotiated rate? The `tone` subcommand plays a sine and reports
//      callbacks seen vs frames expected — the only end-to-end check.
//
// It is a SPIKE: not shipped, not a test gate, one self-contained
// translation unit. It lives under devices/tools/ because tools/ is an
// ALLOW_DIR in tools/check_logging.py — the printed report IS this
// program's output, not a diagnostic.
//
//   asio_probe list
//   asio_probe open <driver-name-or-clsid>
//   asio_probe tone <driver-name-or-clsid> [seconds]
//
// Exit code 0 on success, 1 on failure, 2 on a usage error.
//
// Phase 2 promotes three things out of here, and nothing else:
//   - the registry scan        → already lives in src/asio_driver_list.{h,cc}
//   - the open/create sequence → src/asio_device.{h,cc}
//   - the sample-type fill     → src/asio_convert.h (as proper converters)
// The probe itself stays as a triage tool, exactly as vst3_probe did: it is
// the fastest way to answer "this one driver will not load" without the app.

#if defined( _WIN32 ) && !defined( _WIN64 )
#error "asio_probe is x64-only: the MinGW<->MSVC vtable bet relies on the single x64 calling convention (proposal 35)."
#endif

#include <windows.h>

#include <combaseapi.h>

// SDK headers ONLY — no SDK sources are compiled anywhere in this repo.
// iasiodrv.h pulls in asiosys.h (platform selection) and asio.h (types,
// error codes, sample-type enums) itself.
#include "iasiodrv.h"

#include "asio_driver_list.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

int gWarnings = 0;

void warn( const char *fmt, ... )
{
    std::printf( "    ! " );
    va_list ap;
    va_start( ap, fmt );
    std::vprintf( fmt, ap );
    va_end( ap );
    std::printf( "\n" );
    ++gWarnings;
}

// --- small helpers ------------------------------------------------------------

const char *aseName( ASIOError e )
{
    switch( e ) {
    case ASE_OK:               return "ASE_OK";
    case ASE_SUCCESS:          return "ASE_SUCCESS";
    case ASE_NotPresent:       return "ASE_NotPresent";
    case ASE_HWMalfunction:    return "ASE_HWMalfunction";
    case ASE_InvalidParameter: return "ASE_InvalidParameter";
    case ASE_InvalidMode:      return "ASE_InvalidMode";
    case ASE_SPNotAdvancing:   return "ASE_SPNotAdvancing";
    case ASE_NoClock:          return "ASE_NoClock";
    case ASE_NoMemory:         return "ASE_NoMemory";
    default:                   return "<other>";
    }
}

const char *sampleTypeName( ASIOSampleType t )
{
    switch( t ) {
    case ASIOSTInt16MSB:    return "Int16MSB";
    case ASIOSTInt24MSB:    return "Int24MSB";
    case ASIOSTInt32MSB:    return "Int32MSB";
    case ASIOSTFloat32MSB:  return "Float32MSB";
    case ASIOSTFloat64MSB:  return "Float64MSB";
    case ASIOSTInt32MSB16:  return "Int32MSB16";
    case ASIOSTInt32MSB18:  return "Int32MSB18";
    case ASIOSTInt32MSB20:  return "Int32MSB20";
    case ASIOSTInt32MSB24:  return "Int32MSB24";
    case ASIOSTInt16LSB:    return "Int16LSB";
    case ASIOSTInt24LSB:    return "Int24LSB";
    case ASIOSTInt32LSB:    return "Int32LSB";
    case ASIOSTFloat32LSB:  return "Float32LSB";
    case ASIOSTFloat64LSB:  return "Float64LSB";
    case ASIOSTInt32LSB16:  return "Int32LSB16";
    case ASIOSTInt32LSB18:  return "Int32LSB18";
    case ASIOSTInt32LSB20:  return "Int32LSB20";
    case ASIOSTInt32LSB24:  return "Int32LSB24";
    default:                return "<unknown>";
    }
}

// The output types the backend will support (proposal 35): little-endian
// integer/float only. MSB variants exist in the enum but no x64 interface
// ships them; the backend rejects them at open, so the probe flags them.
bool typeSupported( ASIOSampleType t )
{
    return t == ASIOSTInt16LSB || t == ASIOSTInt24LSB || t == ASIOSTInt32LSB
        || t == ASIOSTFloat32LSB || t == ASIOSTFloat64LSB;
}

std::string lower( std::string s )
{
    for( char &c : s )
        c = (char)std::tolower( (unsigned char)c );
    return s;
}

// --- driver resolution --------------------------------------------------------

// Exact CLSID match first, then case-insensitive substring on name and
// description. Ambiguity is an error, not a guess — the ids feed a real
// backend later, and "asio" matching three drivers must not pick one.
bool resolveDriver( const std::string &arg, audio::AsioDriverEntry &out )
{
    const std::vector<audio::AsioDriverEntry> all = audio::scanAsioDrivers();
    if( all.empty() ) {
        std::printf( "no ASIO drivers registered (HKLM\\SOFTWARE\\ASIO is empty or absent)\n" );
        return false;
    }

    for( const audio::AsioDriverEntry &e : all ) {
        if( lower( e.clsid ) == lower( arg ) ) {
            out = e;
            return true;
        }
    }

    std::vector<audio::AsioDriverEntry> hits;
    const std::string needle = lower( arg );
    for( const audio::AsioDriverEntry &e : all ) {
        if( lower( e.name ).find( needle ) != std::string::npos ||
            lower( e.description ).find( needle ) != std::string::npos )
            hits.push_back( e );
    }

    if( hits.size() == 1 ) {
        out = hits.front();
        return true;
    }
    if( hits.empty() ) {
        std::printf( "no driver matches '%s'; installed:\n", arg.c_str() );
        for( const audio::AsioDriverEntry &e : all )
            std::printf( "  %s\n", e.name.c_str() );
    } else {
        std::printf( "'%s' is ambiguous:\n", arg.c_str() );
        for( const audio::AsioDriverEntry &e : hits )
            std::printf( "  %s\n", e.name.c_str() );
    }
    return false;
}

// --- driver loading -----------------------------------------------------------

// ASIO's COM usage is idiosyncratic: the driver's CLSID doubles as its IID,
// and the object is a plain C++ class with virtuals behind an IUnknown head.
// This one call is the heart of the spike.
IASIO *loadDriver( const audio::AsioDriverEntry &e )
{
    std::wstring w( e.clsid.begin(), e.clsid.end() );
    CLSID clsid;
    if( CLSIDFromString( w.c_str(), &clsid ) != NOERROR ) {
        std::printf( "  FAILED: CLSID '%s' does not parse\n", e.clsid.c_str() );
        return nullptr;
    }

    IASIO *drv = nullptr;
    const HRESULT hr = CoCreateInstance( clsid, nullptr, CLSCTX_INPROC_SERVER,
                                         clsid, (void **)&drv );
    if( FAILED( hr ) || !drv ) {
        std::printf( "  FAILED: CoCreateInstance -> 0x%08lX\n", (unsigned long)hr );
        return nullptr;
    }
    return drv;
}

// --- callbacks ----------------------------------------------------------------
//
// ASIO callbacks carry NO user context pointer, so a process-global active
// driver is not a shortcut here — it is the only possible shape, and the
// production AsioDevice will have the same static trampolines.

struct ToneState {
    IASIO                        *driver = nullptr;
    std::vector<ASIOBufferInfo>   buffers;       // outputs only, for the tone
    std::vector<ASIOSampleType>   types;         // parallel to buffers
    long                          bufferFrames = 0;
    double                        sampleRate = 48000.0;
    bool                          postOutputReady = false;

    double                        phase = 0.0;   // driver thread only
    std::atomic<long>             switches{ 0 };
    std::atomic<long>             timeInfoSwitches{ 0 };
    std::atomic<long>             framesDelivered{ 0 };
    std::atomic<bool>             resetRequested{ false };
    std::atomic<bool>             rateChanged{ false };
    std::atomic<int>              badIndex{ 0 };
};

ToneState *gTone = nullptr;

void fillTone( long index )
{
    ToneState *t = gTone;
    if( !t )
        return;
    if( index != 0 && index != 1 ) {
        // A double-buffer index outside {0,1} means the ABI fell apart —
        // record it, write nothing.
        t->badIndex.store( 1, std::memory_order_relaxed );
        return;
    }

    const double step = 2.0 * 3.14159265358979323846 * 440.0 / t->sampleRate;
    double phase = t->phase;

    for( std::size_t c = 0; c < t->buffers.size(); ++c ) {
        void *dst = t->buffers[c].buffers[index];
        if( !dst )
            continue;
        double p = t->phase;  // same phase on every channel
        switch( t->types[c] ) {
        case ASIOSTInt16LSB: {
            std::int16_t *o = (std::int16_t *)dst;
            for( long i = 0; i < t->bufferFrames; ++i, p += step )
                o[i] = (std::int16_t)( std::sin( p ) * 0.25 * 32767.0 );
            break;
        }
        case ASIOSTInt24LSB: {
            std::uint8_t *o = (std::uint8_t *)dst;
            for( long i = 0; i < t->bufferFrames; ++i, p += step ) {
                const std::int32_t v = (std::int32_t)( std::sin( p ) * 0.25 * 8388607.0 );
                o[i * 3 + 0] = (std::uint8_t)( v & 0xFF );
                o[i * 3 + 1] = (std::uint8_t)( ( v >> 8 ) & 0xFF );
                o[i * 3 + 2] = (std::uint8_t)( ( v >> 16 ) & 0xFF );
            }
            break;
        }
        case ASIOSTInt32LSB: {
            std::int32_t *o = (std::int32_t *)dst;
            for( long i = 0; i < t->bufferFrames; ++i, p += step )
                o[i] = (std::int32_t)( std::sin( p ) * 0.25 * 2147483647.0 );
            break;
        }
        case ASIOSTFloat32LSB: {
            float *o = (float *)dst;
            for( long i = 0; i < t->bufferFrames; ++i, p += step )
                o[i] = (float)( std::sin( p ) * 0.25 );
            break;
        }
        case ASIOSTFloat64LSB: {
            double *o = (double *)dst;
            for( long i = 0; i < t->bufferFrames; ++i, p += step )
                o[i] = std::sin( p ) * 0.25;
            break;
        }
        default:
            // Unsupported type: leave whatever the driver zeroed. The open
            // report already flagged it.
            break;
        }
        phase = p;
    }

    t->phase = phase;
    t->framesDelivered.fetch_add( t->bufferFrames, std::memory_order_relaxed );

    if( t->postOutputReady )
        t->driver->outputReady();
}

void cbBufferSwitch( long index, ASIOBool /*directProcess*/ )
{
    if( gTone )
        gTone->switches.fetch_add( 1, std::memory_order_relaxed );
    fillTone( index );
}

ASIOTime *cbBufferSwitchTimeInfo( ASIOTime *params, long index, ASIOBool /*directProcess*/ )
{
    if( gTone )
        gTone->timeInfoSwitches.fetch_add( 1, std::memory_order_relaxed );
    fillTone( index );
    return params;
}

void cbSampleRateDidChange( ASIOSampleRate /*rate*/ )
{
    if( gTone )
        gTone->rateChanged.store( true, std::memory_order_relaxed );
}

// Which callback entry point to negotiate for. See kAsioSupportsTimeInfo
// below: this is a HOST choice, and flipping it is what exercises the legacy
// path without installing a second driver.
bool gAnswerTimeInfo = true;

long cbAsioMessage( long selector, long value, void * /*message*/, double * /*opt*/ )
{
    switch( selector ) {
    case kAsioSelectorSupported:
        if( value == kAsioSupportsTimeInfo )
            return gAnswerTimeInfo ? 1 : 0;
        return ( value == kAsioEngineVersion || value == kAsioResetRequest ) ? 1 : 0;
    case kAsioEngineVersion:
        return 2;
    case kAsioSupportsTimeInfo:
        // Answer YES so a modern driver takes the bufferSwitchTimeInfo path —
        // the one the production backend will use.
        //
        // `--no-timeinfo` answers NO instead, which is the ONLY way to reach
        // the plain-bufferSwitch path on a driver that supports both: which
        // entry point a driver calls is OUR decision, not a property of the
        // driver, so covering the legacy path needs no second driver — it
        // needs this flag. Measured on the US-16x08: 0/356 with the flag off,
        // and the reverse with it on.
        return gAnswerTimeInfo ? 1 : 0;
    case kAsioResetRequest:
        if( gTone )
            gTone->resetRequested.store( true, std::memory_order_relaxed );
        return 1;
    default:
        return 0;
    }
}

// --- subcommands --------------------------------------------------------------

constexpr double kRateTable[] = { 32000.0, 44100.0, 48000.0, 88200.0,
                                  96000.0, 176400.0, 192000.0 };

int cmdList()
{
    const std::vector<audio::AsioDriverEntry> all = audio::scanAsioDrivers();
    if( all.empty() ) {
        std::printf( "no ASIO drivers registered (HKLM\\SOFTWARE\\ASIO is empty or absent)\n" );
        return 1;
    }
    for( const audio::AsioDriverEntry &e : all ) {
        std::printf( "%-32s %s\n", e.name.c_str(), e.clsid.c_str() );
        if( e.description != e.name )
            std::printf( "%-32s (%s)\n", "", e.description.c_str() );
    }
    std::printf( "%zu driver(s)\n", all.size() );
    return 0;
}

// Shared by `open` and `tone`: CoCreate, init, report the static facts.
// Returns nullptr on failure. On success the caller owns the Release().
IASIO *openAndReport( const audio::AsioDriverEntry &e, long *outCh, long *bufPreferred )
{
    std::printf( "=== %s\n", e.name.c_str() );
    std::printf( "  clsid  : %s\n", e.clsid.c_str() );

    IASIO *drv = loadDriver( e );
    if( !drv )
        return nullptr;

    // init takes a window handle; drivers park their control panel on it and
    // some (ASIO4ALL) refuse a null. The desktop window is the conventional
    // stand-in for a windowless host.
    if( drv->init( GetDesktopWindow() ) != ASIOTrue ) {
        char err[128] = { 0 };
        drv->getErrorMessage( err );
        err[sizeof( err ) - 1] = 0;
        std::printf( "  FAILED: init(): %s\n", err[0] ? err : "(no error message)" );
        drv->Release();
        return nullptr;
    }

    char name[64] = { 0 };
    drv->getDriverName( name );
    name[sizeof( name ) - 1] = 0;
    std::printf( "  driver : '%s'  version %ld\n", name, drv->getDriverVersion() );
    // Garbage here — wrong name, absurd version — is the first tell of a
    // vtable-layout mismatch, before anything crashes.
    if( !name[0] )
        warn( "empty driver name — SUSPECT VTABLE MISMATCH" );

    long nIn = 0, nOut = 0;
    ASIOError r = drv->getChannels( &nIn, &nOut );
    std::printf( "  chans  : %ld in, %ld out  (%s)\n", nIn, nOut, aseName( r ) );
    if( r != ASE_OK || nIn < 0 || nOut < 0 || nIn > 4096 || nOut > 4096 ) {
        warn( "implausible channel counts — SUSPECT VTABLE/STRUCT MISMATCH" );
        drv->Release();
        return nullptr;
    }

    long bMin = 0, bMax = 0, bPref = 0, bGran = 0;
    r = drv->getBufferSize( &bMin, &bMax, &bPref, &bGran );
    std::printf( "  buffer : min %ld, max %ld, preferred %ld, granularity %ld  (%s)\n",
                 bMin, bMax, bPref, bGran, aseName( r ) );

    ASIOSampleRate rate = 0;
    drv->getSampleRate( &rate );
    std::printf( "  rate   : current %.0f;  supported:", (double)rate );
    for( double cand : kRateTable )
        if( drv->canSampleRate( cand ) == ASE_OK )
            std::printf( " %.0f", cand );
    std::printf( "\n" );

    // Channel info: the first few of each direction. ASIOChannelInfo crossing
    // the boundary intact (plausible type enum, readable name) is the POD
    // layout check.
    for( int dir = 0; dir < 2; ++dir ) {
        const long n = dir == 0 ? nIn : nOut;
        for( long i = 0; i < n && i < 8; ++i ) {
            ASIOChannelInfo ci;
            std::memset( &ci, 0, sizeof( ci ) );
            ci.channel = i;
            ci.isInput = dir == 0 ? ASIOTrue : ASIOFalse;
            if( drv->getChannelInfo( &ci ) != ASE_OK )
                continue;
            ci.name[sizeof( ci.name ) - 1] = 0;
            std::printf( "    %-3s [%ld] %-24s %s%s\n", dir == 0 ? "in" : "out", i,
                         ci.name, sampleTypeName( ci.type ),
                         typeSupported( ci.type ) ? "" : "  [UNSUPPORTED TYPE]" );
            if( !typeSupported( ci.type ) )
                warn( "channel reports %s — the backend supports LSB int/float only",
                      sampleTypeName( ci.type ) );
        }
        if( ( dir == 0 ? nIn : nOut ) > 8 )
            std::printf( "    ... %ld more\n", ( dir == 0 ? nIn : nOut ) - 8 );
    }

    if( outCh )
        *outCh = nOut;
    if( bufPreferred )
        *bufPreferred = bPref;
    return drv;
}

int cmdOpen( const std::string &arg )
{
    audio::AsioDriverEntry e;
    if( !resolveDriver( arg, e ) )
        return 1;

    long nOut = 0, bPref = 0;
    IASIO *drv = openAndReport( e, &nOut, &bPref );
    if( !drv )
        return 1;

    // Latencies are only meaningful once buffers exist (many drivers return
    // ASE_NotPresent before createBuffers), so create a minimal set: first
    // two outputs, preferred size, with the real callback table — a driver
    // may probe the pointers even before start().
    ToneState tone;
    tone.driver = drv;
    gTone = &tone;

    ASIOCallbacks cbs;
    cbs.bufferSwitch = cbBufferSwitch;
    cbs.sampleRateDidChange = cbSampleRateDidChange;
    cbs.asioMessage = cbAsioMessage;
    cbs.bufferSwitchTimeInfo = cbBufferSwitchTimeInfo;

    const long useCh = nOut < 2 ? nOut : 2;
    std::vector<ASIOBufferInfo> infos( (std::size_t)useCh );
    for( long i = 0; i < useCh; ++i ) {
        infos[(std::size_t)i].isInput = ASIOFalse;
        infos[(std::size_t)i].channelNum = i;
        infos[(std::size_t)i].buffers[0] = infos[(std::size_t)i].buffers[1] = nullptr;
    }

    ASIOError r = useCh > 0
                      ? drv->createBuffers( infos.data(), useCh, bPref, &cbs )
                      : ASE_InvalidParameter;
    std::printf( "  create : %ld ch x %ld frames -> %s\n", useCh, bPref, aseName( r ) );

    int rc = 0;
    if( r == ASE_OK ) {
        long lIn = 0, lOut = 0;
        r = drv->getLatencies( &lIn, &lOut );
        std::printf( "  latency: input %ld frames, output %ld frames  (%s)\n",
                     lIn, lOut, aseName( r ) );
        std::printf( "  outputReady: %s\n",
                     drv->outputReady() == ASE_OK ? "supported" : "not supported" );
        drv->disposeBuffers();
    } else {
        warn( "createBuffers failed" );
        rc = 1;
    }

    gTone = nullptr;
    drv->Release();
    std::printf( "  RESULT : %s\n", rc == 0 ? "open lifecycle OK" : "FAILED" );
    return rc;
}

int cmdTone( const std::string &arg, int seconds )
{
    audio::AsioDriverEntry e;
    if( !resolveDriver( arg, e ) )
        return 1;

    long nOut = 0, bPref = 0;
    IASIO *drv = openAndReport( e, &nOut, &bPref );
    if( !drv )
        return 1;
    if( nOut < 1 ) {
        std::printf( "  FAILED: driver has no outputs\n" );
        drv->Release();
        return 1;
    }

    // Prefer 48 kHz (the project default) when the driver can do it; else
    // keep whatever it is set to.
    if( drv->canSampleRate( 48000.0 ) == ASE_OK )
        drv->setSampleRate( 48000.0 );
    ASIOSampleRate rate = 48000.0;
    drv->getSampleRate( &rate );

    ToneState tone;
    tone.driver = drv;
    tone.sampleRate = (double)rate;

    ASIOCallbacks cbs;
    cbs.bufferSwitch = cbBufferSwitch;
    cbs.sampleRateDidChange = cbSampleRateDidChange;
    cbs.asioMessage = cbAsioMessage;
    cbs.bufferSwitchTimeInfo = cbBufferSwitchTimeInfo;

    const long useCh = nOut < 2 ? nOut : 2;
    tone.buffers.resize( (std::size_t)useCh );
    tone.types.resize( (std::size_t)useCh );
    for( long i = 0; i < useCh; ++i ) {
        tone.buffers[(std::size_t)i].isInput = ASIOFalse;
        tone.buffers[(std::size_t)i].channelNum = i;
        tone.buffers[(std::size_t)i].buffers[0] = nullptr;
        tone.buffers[(std::size_t)i].buffers[1] = nullptr;
    }

    ASIOError r = drv->createBuffers( tone.buffers.data(), useCh, bPref, &cbs );
    if( r != ASE_OK ) {
        std::printf( "  FAILED: createBuffers -> %s\n", aseName( r ) );
        drv->Release();
        return 1;
    }
    tone.bufferFrames = bPref;

    for( long i = 0; i < useCh; ++i ) {
        ASIOChannelInfo ci;
        std::memset( &ci, 0, sizeof( ci ) );
        ci.channel = i;
        ci.isInput = ASIOFalse;
        drv->getChannelInfo( &ci );
        tone.types[(std::size_t)i] = ci.type;
    }
    tone.postOutputReady = drv->outputReady() == ASE_OK;

    long lIn = 0, lOut = 0;
    drv->getLatencies( &lIn, &lOut );
    std::printf( "  run    : %.0f Hz, %ld frames/buffer, latency out %ld, outputReady %s,"
                 " timeInfo %s\n",
                 (double)rate, bPref, lOut, tone.postOutputReady ? "yes" : "no",
                 gAnswerTimeInfo ? "requested" : "DECLINED (--no-timeinfo)" );

    // Publish the state BEFORE start — a driver may fire the first
    // bufferSwitch from inside start() itself.
    gTone = &tone;

    r = drv->start();
    if( r != ASE_OK ) {
        std::printf( "  FAILED: start -> %s\n", aseName( r ) );
        gTone = nullptr;
        drv->disposeBuffers();
        drv->Release();
        return 1;
    }

    for( int elapsed = 0; elapsed < seconds * 10; ++elapsed ) {
        std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
        if( tone.resetRequested.load( std::memory_order_relaxed ) ) {
            std::printf( "  note   : driver sent kAsioResetRequest; stopping early\n" );
            break;
        }
    }

    r = drv->stop();
    std::printf( "  stop   : %s\n", aseName( r ) );

    // The production stop fence: after stop() returns, no further callback
    // may arrive. Give a misbehaving driver a beat to prove otherwise.
    const long switchesAtStop =
        tone.switches.load( std::memory_order_relaxed ) +
        tone.timeInfoSwitches.load( std::memory_order_relaxed );
    std::this_thread::sleep_for( std::chrono::milliseconds( 200 ) );
    const long switchesAfter =
        tone.switches.load( std::memory_order_relaxed ) +
        tone.timeInfoSwitches.load( std::memory_order_relaxed );

    drv->disposeBuffers();
    gTone = nullptr;

    const long delivered = tone.framesDelivered.load( std::memory_order_relaxed );
    const long expected = (long)( (double)rate * seconds );
    std::printf( "  frames : %ld delivered, ~%ld expected  (%.0f%%)\n", delivered, expected,
                 expected > 0 ? 100.0 * (double)delivered / (double)expected : 0.0 );
    std::printf( "  calls  : %ld bufferSwitch, %ld bufferSwitchTimeInfo\n",
                 tone.switches.load( std::memory_order_relaxed ),
                 tone.timeInfoSwitches.load( std::memory_order_relaxed ) );
    if( tone.rateChanged.load( std::memory_order_relaxed ) )
        std::printf( "  note   : driver reported sampleRateDidChange during the run\n" );

    // Did the driver honour the negotiation? Phase 2 implements BOTH entry
    // points into one body, so neither answer here is a failure — but a
    // driver that ignores the answer is worth knowing about before the
    // backend relies on it.
    const long plainCalls = tone.switches.load( std::memory_order_relaxed );
    const long infoCalls  = tone.timeInfoSwitches.load( std::memory_order_relaxed );
    if( !gAnswerTimeInfo && infoCalls > 0 )
        warn( "declined kAsioSupportsTimeInfo and the driver called "
              "bufferSwitchTimeInfo %ld times anyway — it ignores the negotiation",
              infoCalls );
    if( gAnswerTimeInfo && infoCalls == 0 && plainCalls > 0 )
        std::printf( "  note   : driver has no timeInfo support; it used plain "
                     "bufferSwitch despite the offer\n" );
    if( !gAnswerTimeInfo && plainCalls > 0 && infoCalls == 0 )
        std::printf( "  note   : legacy bufferSwitch path exercised end to end\n" );

    bool ok = true;
    if( tone.badIndex.load( std::memory_order_relaxed ) ) {
        warn( "bufferSwitch index outside {0,1} — SUSPECT ABI MISMATCH" );
        ok = false;
    }
    if( switchesAfter != switchesAtStop ) {
        warn( "callbacks arrived AFTER stop() returned (%ld late) — the backend's stop "
              "fence must handle this driver",
              switchesAfter - switchesAtStop );
        // Reported, but not a gate failure: the fence is designed for it.
    }
    // Under 50% delivery means the stream never really ran (a couple of
    // buffers of slack around start/stop is normal).
    if( delivered < expected / 2 ) {
        warn( "delivered far fewer frames than the clock implies — stream did not run" );
        ok = false;
    }

    drv->Release();
    std::printf( "  RESULT : %s\n", ok ? "tone lifecycle OK" : "FAILED" );
    return ok ? 0 : 1;
}

}  // namespace

int main( int argc, char **argv )
{
    // Never answer a broken driver with a modal dialog — same call as
    // vst3_probe and plugin_probe, for the same reason: a spike must be
    // runnable unattended.
    SetErrorMode( SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX
                  | SEM_NOOPENFILEERRORBOX );

    // Flags are stripped first so they may appear anywhere; what is left is
    // the positional command line.
    std::vector<std::string> av;
    for( int i = 1; i < argc; ++i ) {
        const std::string a = argv[i];
        if( a == "--no-timeinfo" )
            gAnswerTimeInfo = false;
        else
            av.push_back( a );
    }

    if( av.empty() ) {
        std::printf(
            "usage: asio_probe list\n"
            "       asio_probe open <driver-name-or-clsid> [--no-timeinfo]\n"
            "       asio_probe tone <driver-name-or-clsid> [seconds] [--no-timeinfo]\n"
            "\n"
            "Proposal 35 Phase 1 ABI gate: drives an MSVC-built ASIO driver from this\n"
            "MinGW-built host — enumerate -> CoCreate -> init -> channels/rates/buffers\n"
            "-> createBuffers -> start -> bufferSwitch -> stop -> dispose -> Release.\n"
            "\n"
            "--no-timeinfo answers NO to kAsioSupportsTimeInfo, so a driver that\n"
            "supports both entry points takes the plain bufferSwitch path instead.\n"
            "Which one a driver calls is the HOST's choice, so this covers the legacy\n"
            "path without needing a second driver installed.\n" );
        return 2;
    }

    // STA, per ASIO convention: drivers are in-proc servers written for a
    // plain message-pumping host thread.
    const HRESULT hr = CoInitialize( nullptr );
    if( FAILED( hr ) ) {
        std::printf( "CoInitialize failed: 0x%08lX\n", (unsigned long)hr );
        return 1;
    }

    const std::string cmd = av[0];
    int rc = 2;
    if( cmd == "list" ) {
        rc = cmdList();
    } else if( cmd == "open" && av.size() >= 2 ) {
        rc = cmdOpen( av[1] );
    } else if( cmd == "tone" && av.size() >= 2 ) {
        int seconds = 2;
        if( av.size() >= 3 ) {
            seconds = std::atoi( av[2].c_str() );
            if( seconds < 1 || seconds > 60 )
                seconds = 2;
        }
        rc = cmdTone( av[1], seconds );
    } else {
        std::printf( "unknown or incomplete command '%s' — run without arguments for usage\n",
                     cmd.c_str() );
    }

    CoUninitialize();

    if( gWarnings )
        std::printf( "%d warning(s) — see the '!' lines above.\n", gWarnings );
    if( rc == 0 && ( cmd == "open" || cmd == "tone" ) )
        std::printf( "GATE PASSED: the MinGW <-> MSVC ASIO ABI works here.\n" );
    else if( rc == 1 && ( cmd == "open" || cmd == "tone" ) )
        std::printf( "GATE FAILED: see above.\n" );
    return rc;
}
