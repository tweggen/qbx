// sidecar_test.cc — unit tests for the sidecar module (proposal 27).
//
// House pattern (see pages/tests/io_vector_test.cc): plain main(), no test
// framework, std::cout/std::cerr allowed in tests. A CHECK(cond, msg) helper
// records every failure so all tests run; main returns the failure count
// (0 == success). Written strictly against the header contracts:
//   tw/sidecar/twqaf.h, tw/sidecar/twsidecarstore.h, tw/sidecar/twaspects.h,
//   tw/core/twcontenthash.h
//
// Deterministic: fixed seeds, an in-tree LCG (never rand()), no time-based
// logic beyond two deliberate mtime sleeps in the eviction section. Every temp
// artifact is removed at the end (RAII guard) even when a CHECK fails.

#include "tw/sidecar/twqaf.h"
#include "tw/sidecar/twsidecarstore.h"
#include "tw/sidecar/twaspects.h"
#include "tw/sidecar/twgrooveaspect.h"
#include "tw/core/twcontenthash.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// getpid — the only platform header we reach for, and only to name a unique
// temp dir as the task spec requires ("smaragd_sidecar_test_<pid>").
#ifdef _WIN32
#  include <process.h>
static int selfPid() { return _getpid(); }
#else
#  include <unistd.h>
static int selfPid() { return static_cast<int>(getpid()); }
#endif

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// CHECK helper
// ---------------------------------------------------------------------------
static int g_fails = 0;
#define CHECK( cond, msg )                                                     \
    do {                                                                      \
        if ( !( cond ) ) {                                                    \
            std::cerr << "FAIL: " << ( msg ) << "  [" << __FILE__ << ":"      \
                      << __LINE__ << "]\n";                                   \
            ++g_fails;                                                        \
        }                                                                     \
    } while ( 0 )

// ---------------------------------------------------------------------------
// RAII cleanup: remove every registered path (file or dir) at scope exit.
// ---------------------------------------------------------------------------
struct Cleanup {
    std::vector<fs::path> paths;
    void                  add( const fs::path &p ) { paths.push_back( p ); }
    ~Cleanup() {
        for ( const auto &p : paths ) {
            std::error_code ec;
            fs::remove_all( p, ec ); // best-effort; ignore errors
        }
    }
};
static Cleanup g_cleanup;

// ---------------------------------------------------------------------------
// Small deterministic helpers
// ---------------------------------------------------------------------------

// Numerical-Recipes LCG, low byte per step. Fixed seed -> fixed bytes.
static std::vector<uint8_t> lcgBytes( uint32_t seed, size_t n ) {
    std::vector<uint8_t> out( n );
    uint32_t             x = seed;
    for ( size_t i = 0; i < n; ++i ) {
        x       = x * 1664525u + 1013904223u;
        out[i]  = static_cast<uint8_t>( x >> 16 );
    }
    return out;
}

static std::string hex16( uint64_t v ) {
    char buf[17];
    std::snprintf( buf, sizeof buf, "%016llx",
                   static_cast<unsigned long long>( v ) );
    return std::string( buf );
}

// Patch a single byte in place via raw fstream.
static void patchByte( const fs::path &p, uint64_t off, uint8_t val ) {
    std::fstream fsx( p, std::ios::binary | std::ios::in | std::ios::out );
    fsx.seekp( static_cast<std::streamoff>( off ) );
    char c = static_cast<char>( val );
    fsx.write( &c, 1 );
}

// Copy the round-trip file to a fresh, cleanup-registered path.
static fs::path freshCopy( const fs::path &src, const std::string &tag ) {
    fs::path dst =
        src.parent_path() / ( src.stem().string() + "_" + tag + ".qaf" );
    std::error_code ec;
    fs::remove( dst, ec );
    fs::copy_file( src, dst, fs::copy_options::overwrite_existing, ec );
    g_cleanup.add( dst );
    return dst;
}

// ===========================================================================
// Section 1 — twHashBuffer / twContentHash properties
// ===========================================================================
static void section1_hash() {
    std::cout << "== Section 1: content hash ==\n";

    const std::vector<uint8_t> buf = lcgBytes( 0xC0FFEEu, 500 );

    // Determinism: same buffer twice -> equal.
    twContentHash h1 = twHashBuffer( buf.data(), buf.size() );
    twContentHash h2 = twHashBuffer( buf.data(), buf.size() );
    CHECK( h1 == h2, "hash is deterministic (same buffer twice)" );
    CHECK( !h1.isNull(), "hash of real data is non-null" );

    // Sensitivity: flip one byte -> different.
    {
        std::vector<uint8_t> flipped = buf;
        flipped[123] ^= 0xFF;
        twContentHash hf = twHashBuffer( flipped.data(), flipped.size() );
        CHECK( hf != h1, "flip one byte changes the digest" );
    }

    // Sensitivity: append one byte -> different.
    {
        std::vector<uint8_t> appended = buf;
        appended.push_back( 0x7A );
        twContentHash ha = twHashBuffer( appended.data(), appended.size() );
        CHECK( ha != h1, "append one byte changes the digest" );
    }

    // Empty input works and is non-null under the default seed.
    {
        twContentHash he = twHashBuffer( "", 0 );
        CHECK( !he.isNull(), "empty-input digest is non-null (default seed)" );
    }

    // Different seeds -> different digests.
    {
        twContentHash sa =
            twHashBuffer( buf.data(), buf.size(), 0x1111111111111111ULL );
        twContentHash sb =
            twHashBuffer( buf.data(), buf.size(), 0x2222222222222222ULL );
        CHECK( sa != sb, "different seeds produce different digests" );
        CHECK( sa != h1, "seeded digest differs from default-seed digest" );
    }

    // toHex(): 32 lowercase hex chars; round-trips with hi|lo formatting.
    {
        std::string hex = h1.toHex();
        CHECK( hex.size() == 32, "toHex() is 32 chars" );
        bool lower = true;
        for ( char c : hex )
            if ( !( ( c >= '0' && c <= '9' ) || ( c >= 'a' && c <= 'f' ) ) )
                lower = false;
        CHECK( lower, "toHex() is all lowercase hex" );

        char expect[33];
        std::snprintf( expect, sizeof expect, "%016llx%016llx",
                       static_cast<unsigned long long>( h1.hi ),
                       static_cast<unsigned long long>( h1.lo ) );
        CHECK( hex == std::string( expect ),
               "toHex() == hi|lo (hi first, %016llx each)" );
    }

    // Golden pin: byte[i] == i % 256 over 10000 bytes, default seed. The value
    // was cross-checked against an independent MurmurHash3 x64-128 reference
    // implementation (2026-07-24). It guards the digest against accidental
    // algorithm changes — sidecar keys on disk would all orphan silently.
    {
        std::vector<uint8_t> golden( 10000 );
        for ( size_t i = 0; i < golden.size(); ++i )
            golden[i] = static_cast<uint8_t>( i % 256 );
        twContentHash hg = twHashBuffer( golden.data(), golden.size() );
        std::cout << "GOLDEN preview: " << hg.toHex() << "\n";
        CHECK( hg.toHex() == "4767e836363c3de4c6ff91a77dce60db",
               "golden digest pinned (MurmurHash3 x64-128, seed 'smaragd')" );
    }
}

// ===========================================================================
// Section 2 — QAF round-trip
// ===========================================================================
static fs::path section2_roundtrip() {
    std::cout << "== Section 2: QAF round-trip ==\n";

    fs::path path = fs::temp_directory_path() /
                    ( "smaragd_sidecar_rt_" + std::to_string( selfPid() ) +
                      ".qaf" );
    g_cleanup.add( path );

    twQafInfo info;
    info.aspectId     = "test.aspect";
    info.aspectVersion = 3;
    info.contentHash.lo = 0x0123456789abcdefULL;
    info.contentHash.hi = 0xfedcba9876543210ULL;
    info.sourceRate   = 48000;
    info.channels     = 2;
    info.sourceFrames = 12345;
    info.recordStride = 8;
    info.recordCount  = 100;               // 8 * 100 == 800 == payloadLen
    info.hopFrames    = 256;
    info.params       = lcgBytes( 0xA5A5u, 17 ); // 17 arbitrary bytes

    const std::vector<uint8_t> payload = lcgBytes( 0xBEEF01u, 800 );

    bool wrote = twQafWriter::write( path, info, payload.data(),
                                     payload.size() );
    CHECK( wrote, "twQafWriter::write succeeds" );

    twQafReader rd;
    bool        opened = rd.open( path );
    CHECK( opened, "twQafReader::open succeeds on written file" );
    CHECK( rd.isOpen(), "reader reports open after successful open" );

    const twQafInfo &ri = rd.info();
    CHECK( ri.aspectId == "test.aspect", "round-trip aspectId" );
    CHECK( ri.aspectVersion == 3u, "round-trip aspectVersion" );
    CHECK( ri.contentHash == info.contentHash, "round-trip contentHash" );
    CHECK( ri.sourceRate == 48000u, "round-trip sourceRate" );
    CHECK( ri.channels == 2u, "round-trip channels" );
    CHECK( ri.sourceFrames == 12345u, "round-trip sourceFrames" );
    CHECK( ri.recordStride == 8u, "round-trip recordStride" );
    CHECK( ri.recordCount == 100u, "round-trip recordCount" );
    CHECK( ri.hopFrames == 256u, "round-trip hopFrames" );
    CHECK( ri.params == info.params, "round-trip params blob" );
    CHECK( ri.payloadLen == 800u, "reader reports payloadLen == 800" );

    // readAllPayload == original.
    {
        std::vector<uint8_t> all;
        bool                 ok = rd.readAllPayload( all );
        CHECK( ok, "readAllPayload succeeds" );
        CHECK( all == payload, "readAllPayload equals original payload" );
    }

    // readPayload at various offsets/lens matches slices.
    struct Slice { uint64_t off, len; };
    const Slice slices[] = { { 0, 100 }, { 1, 10 }, { 799, 1 }, { 400, 200 } };
    for ( const Slice &s : slices ) {
        std::vector<uint8_t> got( s.len );
        bool ok = rd.readPayload( got.data(), s.off, s.len );
        CHECK( ok, "readPayload in-bounds succeeds" );
        std::vector<uint8_t> want( payload.begin() + s.off,
                                   payload.begin() + s.off + s.len );
        CHECK( got == want, "readPayload slice matches" );
    }

    // Out-of-bounds readPayload (offset+len > 800) returns false.
    {
        std::vector<uint8_t> junk( 4 );
        CHECK( !rd.readPayload( junk.data(), 799, 2 ),
               "readPayload out-of-bounds (799+2) returns false" );
        CHECK( !rd.readPayload( junk.data(), 800, 1 ),
               "readPayload out-of-bounds (800+1) returns false" );
        std::vector<uint8_t> big( 401 );
        CHECK( !rd.readPayload( big.data(), 400, 401 ),
               "readPayload out-of-bounds (400+401) returns false" );
    }

    // readRecords(dest, 50, 25) with stride 8 == payload bytes [400, 600).
    {
        std::vector<uint8_t> got( 25 * 8 );
        bool ok = rd.readRecords( got.data(), 50, 25 );
        CHECK( ok, "readRecords(50,25) succeeds" );
        std::vector<uint8_t> want( payload.begin() + 400,
                                   payload.begin() + 600 );
        CHECK( got == want, "readRecords(50,25) equals payload[400,600)" );
    }

    return path;
}

// ===========================================================================
// Section 3 — validation failures (each on a fresh copy, patched raw)
// ===========================================================================
static void section3_validation( const fs::path &roundtrip ) {
    std::cout << "== Section 3: validation failures ==\n";

    // (a) corrupt magic byte 0.
    {
        fs::path p = freshCopy( roundtrip, "badmagic" );
        patchByte( p, 0, 'X' ); // was 'Q'
        twQafReader rd;
        CHECK( !rd.open( p ), "open fails on corrupt magic" );
        CHECK( !rd.isOpen(), "reader not open after magic failure" );
    }

    // (b) flip one byte inside the CRC-covered region [0,140) (offset 60 is
    //     within sourceFrames).
    {
        fs::path p = freshCopy( roundtrip, "badcrc" );
        std::fstream fsx( p, std::ios::binary | std::ios::in | std::ios::out );
        fsx.seekg( 60 );
        char orig = 0;
        fsx.read( &orig, 1 );
        fsx.seekp( 60 );
        char flipped = static_cast<char>( orig ^ 0xFF );
        fsx.write( &flipped, 1 );
        fsx.close();
        twQafReader rd;
        CHECK( !rd.open( p ), "open fails on CRC-region byte flip" );
        CHECK( !rd.isOpen(), "reader not open after CRC failure" );
    }

    // (c) truncate the file 10 bytes short.
    {
        fs::path        p = freshCopy( roundtrip, "trunc" );
        std::error_code ec;
        auto            sz = fs::file_size( p, ec );
        CHECK( !ec && sz > 10, "truncation precondition: file large enough" );
        fs::resize_file( p, sz - 10, ec );
        CHECK( !ec, "resize_file succeeds" );
        twQafReader rd;
        CHECK( !rd.open( p ), "open fails on truncated file" );
        CHECK( !rd.isOpen(), "reader not open after truncation failure" );
    }

    // (d) formatVersion = 2 (CRC now also mismatches — acceptable).
    {
        fs::path p = freshCopy( roundtrip, "badver" );
        patchByte( p, 4, 2 ); // formatVersion low byte, was 1
        twQafReader rd;
        CHECK( !rd.open( p ), "open fails on formatVersion == 2" );
        CHECK( !rd.isOpen(), "reader not open after version failure" );
    }
}

// ===========================================================================
// Helpers for the store sections
// ===========================================================================
static twQafInfo makePreviewInfo( const twContentHash &ch, uint32_t projectRate,
                                  uint64_t recCount ) {
    twQafInfo info;
    info.aspectId     = twAspect::PreviewPeaks;
    info.aspectVersion = twAspect::PreviewPeaksVersion;
    info.contentHash  = ch;
    info.sourceRate   = 48000;
    info.channels     = 2;
    info.sourceFrames = recCount;
    info.recordStride = 2;                // preview.peaks: 2 bytes/record
    info.recordCount  = recCount;
    info.hopFrames    = 256;
    info.params.resize( 4 );
    std::memcpy( info.params.data(), &projectRate, 4 );
    return info;
}

static twContentHash hashOfInt( uint64_t v ) {
    return twHashBuffer( &v, sizeof v );
}

// ===========================================================================
// Section 4 — store lifecycle
// ===========================================================================
static void section4_store( const fs::path &pidDir ) {
    std::cout << "== Section 4: store lifecycle ==\n";

    twSidecarStore store; // LOCAL instance, not instance()

    // Before setRoot: disabled.
    CHECK( !store.enabled(), "store disabled before setRoot" );
    {
        twContentHash ch = hashOfInt( 1 );
        CHECK( store.load( ch, twAspect::PreviewPeaks,
                           twAspect::PreviewPeaksVersion, 0 ) == nullptr,
               "load() is nullptr while disabled" );
        twQafInfo            info = makePreviewInfo( ch, 48000, 8 );
        std::vector<uint8_t> pl   = lcgBytes( 0x1u, 16 );
        CHECK( !store.store( info, pl.data(), pl.size() ),
               "store() is a no-op (false) while disabled" );
    }

    store.setRoot( pidDir.string() );
    CHECK( store.enabled(), "store enabled after setRoot" );

    const uint32_t       projectRate = 48000;
    twContentHash        ch          = hashOfInt( 0xABCD );
    twQafInfo            info        = makePreviewInfo( ch, projectRate, 512 );
    std::vector<uint8_t> payload     = lcgBytes( 0x51DEu, 1024 );
    uint64_t             ph =
        twSidecarStore::hashParams( info.params.data(), info.params.size() );

    CHECK( store.store( info, payload.data(), payload.size() ),
           "store() succeeds when enabled" );

    // Exact-identity hit round-trips.
    {
        auto rd = store.load( ch, twAspect::PreviewPeaks,
                              twAspect::PreviewPeaksVersion, ph );
        CHECK( rd && rd->isOpen(), "load() hit on exact identity" );
        if ( rd ) {
            std::vector<uint8_t> got;
            rd->readAllPayload( got );
            CHECK( got == payload, "loaded payload round-trips" );
        }
    }

    // Wrong params hash -> miss (file NOT deleted).
    {
        auto rd = store.load( ch, twAspect::PreviewPeaks,
                              twAspect::PreviewPeaksVersion, ph + 1 );
        CHECK( rd == nullptr, "load() miss on wrong paramsHash" );
    }

    // Wrong content hash -> miss.
    {
        auto rd = store.load( hashOfInt( 0x9999 ), twAspect::PreviewPeaks,
                              twAspect::PreviewPeaksVersion, ph );
        CHECK( rd == nullptr, "load() miss on wrong contentHash" );
    }

    // Wrong aspect version -> miss AND file deleted (version orphaning).
    {
        fs::path onDisk = store.pathFor( ch, twAspect::PreviewPeaks, ph );
        CHECK( fs::exists( onDisk ),
               "file present before version-orphan load" );
        auto rd = store.load( ch, twAspect::PreviewPeaks,
                              twAspect::PreviewPeaksVersion + 1, ph );
        CHECK( rd == nullptr, "load() miss on wrong aspectVersion" );
        CHECK( !fs::exists( onDisk ),
               "orphaned file deleted on version mismatch (rule 1)" );

        // Re-storing brings the file back.
        CHECK( store.store( info, payload.data(), payload.size() ),
               "re-store after orphaning succeeds" );
        CHECK( fs::exists( onDisk ), "file exists again after re-store" );
    }

    // ----------------------------------------------------------------- B8.3
    // THE PREVIEW BUMP, from the direction that matters. The check above orphans
    // a file when the READER asks for a newer version than the file carries;
    // this one is the shape a user's machine is actually in after the update —
    // an OLD file on disk (written at PreviewPeaksVersion - 1, under the
    // channel-0 fold) meeting the CURRENT reader. It must MISS, not be adopted:
    // the v1 bytes are only accidentally right, agreeing with the all-channel
    // fold exactly when channel 0 dominates.
    //
    // Asserted as a miss and a deletion, deliberately not as "the key changed":
    // a changed key proves the two would not collide, and says nothing about
    // what a reader does when it meets the old file.
    {
        twContentHash        oldCh   = hashOfInt( 0x0B8B8 );
        twQafInfo            oldInfo = makePreviewInfo( oldCh, projectRate, 512 );
        oldInfo.aspectVersion        = twAspect::PreviewPeaksVersion - 1;
        oldInfo.channels             = 1;   // what v1 always wrote
        std::vector<uint8_t> oldPayload = lcgBytes( 0xB8B8u, 1024 );
        const uint64_t oldPh = twSidecarStore::hashParams(
            oldInfo.params.data(), oldInfo.params.size() );

        CHECK( store.store( oldInfo, oldPayload.data(), oldPayload.size() ),
               "a v1 preview sidecar can be written (the pre-B8 state)" );
        fs::path onDisk = store.pathFor( oldCh, twAspect::PreviewPeaks, oldPh );
        CHECK( fs::exists( onDisk ), "…and it is on disk" );

        auto rd = store.load( oldCh, twAspect::PreviewPeaks,
                              twAspect::PreviewPeaksVersion, oldPh );
        CHECK( rd == nullptr,
               "an OLD preview sidecar MISSES after the B8 version bump" );
        CHECK( !fs::exists( onDisk ),
               "…and is orphaned off the disk rather than left to be re-read" );

        // The same content at the CURRENT version is a hit, so the miss above is
        // the version and not something broken about this key.
        twQafInfo newInfo = makePreviewInfo( oldCh, projectRate, 512 );
        CHECK( store.store( newInfo, oldPayload.data(), oldPayload.size() ),
               "the same content re-stores at the current version" );
        CHECK( store.load( oldCh, twAspect::PreviewPeaks,
                           twAspect::PreviewPeaksVersion, oldPh ) != nullptr,
               "…and now hits" );
    }
}

// Sum of all *.qaf bytes under root (recursive; store uses <hh>/ subdirs).
static uint64_t qafBytesUnder( const fs::path &root ) {
    uint64_t        total = 0;
    std::error_code ec;
    for ( auto it = fs::recursive_directory_iterator( root, ec );
          it != fs::recursive_directory_iterator(); it.increment( ec ) ) {
        if ( ec )
            break;
        const auto &e = *it;
        if ( e.is_regular_file() && e.path().extension() == ".qaf" )
            total += static_cast<uint64_t>( fs::file_size( e.path() ) );
    }
    return total;
}

// ===========================================================================
// Section 5 — eviction (store rule 3)
// ===========================================================================
static void section5_eviction( const fs::path &pidDir ) {
    std::cout << "== Section 5: eviction ==\n";

    fs::path        root = pidDir / "evict";
    std::error_code ec;
    fs::remove_all( root, ec );
    g_cleanup.add( root );

    twSidecarStore store;
    store.setRoot( root.string() );
    CHECK( store.enabled(), "eviction store enabled" );

    const uint64_t cap = 3 * 1024; // 3 KiB
    store.setSizeCapBytes( cap );
    CHECK( store.sizeCapBytes() == cap, "size cap set to 3 KiB" );

    // Store 6 aspects of ~1 KiB payload, distinct content hashes. Sleep only
    // between the first two so the oldest has a strictly older mtime.
    std::vector<fs::path> paths;
    for ( int i = 0; i < 6; ++i ) {
        twContentHash        ch      = hashOfInt( 0x1000 + i );
        twQafInfo            info    = makePreviewInfo( ch, 48000, 512 );
        std::vector<uint8_t> payload = lcgBytes( 0x2000u + i, 1024 );
        uint64_t             ph      = twSidecarStore::hashParams(
            info.params.data(), info.params.size() );
        paths.push_back( store.pathFor( ch, twAspect::PreviewPeaks, ph ) );
        store.store( info, payload.data(), payload.size() );
        if ( i == 0 )
            std::this_thread::sleep_for( std::chrono::milliseconds( 1100 ) );
    }

    uint64_t after = qafBytesUnder( root );
    CHECK( after <= cap, "total *.qaf bytes <= cap after final store" );
    CHECK( !fs::exists( paths[0] ),
           "oldest file is among those evicted (rule 3, LRU by mtime)" );

    // Open-handle case: store two more, hold an open reader over one, then
    // evict with cap 0.
    twContentHash        chA     = hashOfInt( 0x2000 );
    twContentHash        chB     = hashOfInt( 0x2001 );
    twQafInfo            infoA   = makePreviewInfo( chA, 48000, 512 );
    twQafInfo            infoB   = makePreviewInfo( chB, 48000, 512 );
    std::vector<uint8_t> plA     = lcgBytes( 0x3000u, 1024 );
    std::vector<uint8_t> plB     = lcgBytes( 0x3001u, 1024 );
    uint64_t             phA =
        twSidecarStore::hashParams( infoA.params.data(), infoA.params.size() );

    // Bump the cap so both survive the store() calls, then we drive eviction
    // explicitly below.
    store.setSizeCapBytes( 1024 * 1024 );
    store.store( infoA, plA.data(), plA.size() );
    store.store( infoB, plB.data(), plB.size() );
    fs::path pathA = store.pathFor( chA, twAspect::PreviewPeaks, phA );

    auto R = store.load( chA, twAspect::PreviewPeaks,
                         twAspect::PreviewPeaksVersion, phA );
    CHECK( R && R->isOpen(), "open reader R over file A" );

    store.setSizeCapBytes( 0 );
    store.evictIfNeeded(); // must not crash even with R's handle open
    CHECK( true, "evictIfNeeded() with cap 0 and open handle did not crash" );

#ifdef _WIN32
    // CRT fopen denies delete of an open file: A is skipped, still present.
    CHECK( fs::exists( pathA ),
           "Windows: R's open file survives eviction (skip, rule 3)" );
#endif

    // Store still functional: with a sane cap, a fresh store+load round-trips.
    {
        store.setSizeCapBytes( 1024 * 1024 );
        twContentHash        chC   = hashOfInt( 0x2002 );
        twQafInfo            infoC = makePreviewInfo( chC, 48000, 512 );
        std::vector<uint8_t> plC   = lcgBytes( 0x3002u, 1024 );
        uint64_t             phC   = twSidecarStore::hashParams(
            infoC.params.data(), infoC.params.size() );
        CHECK( store.store( infoC, plC.data(), plC.size() ),
               "store still functional after cap-0 eviction" );
        auto rc = store.load( chC, twAspect::PreviewPeaks,
                              twAspect::PreviewPeaksVersion, phC );
        CHECK( rc && rc->isOpen(),
               "load still functional after cap-0 eviction" );
        if ( rc ) {
            std::vector<uint8_t> got;
            rc->readAllPayload( got );
            CHECK( got == plC, "post-eviction store+load payload round-trips" );
        }
    }

    // Close R, evict again: the previously-skipped file can now go.
    R.reset();
    store.setSizeCapBytes( 0 );
    store.evictIfNeeded();
#ifdef _WIN32
    CHECK( !fs::exists( pathA ),
           "Windows: R's file is deleted once its handle is closed" );
#endif
}

// ===========================================================================
// Section 6 — pathFor spelling
// ===========================================================================
static void section6_pathFor( const fs::path &pidDir ) {
    std::cout << "== Section 6: pathFor spelling ==\n";

    fs::path root = pidDir / "spell";
    g_cleanup.add( root );

    twSidecarStore store;
    store.setRoot( root.string() );

    twContentHash ch        = hashOfInt( 0xF00DBEEF );
    std::string   aspect    = twAspect::PreviewPeaks;
    uint64_t      paramsHash = 0xDEADBEEF12345678ULL;

    std::string hex32 = ch.toHex();
    std::string hh    = hex32.substr( 0, 2 );

    fs::path expected =
        root / hh / ( hex32 + "." + aspect + "." + hex16( paramsHash ) +
                      ".qaf" );

    fs::path actual = store.pathFor( ch, aspect, paramsHash );

    CHECK( actual.generic_string() == expected.generic_string(),
           "pathFor == <root>/<hh>/<hex32>.<aspect>.<hex16>.qaf" );
}

// ===========================================================================
// Section 7 — proposal 40 "Feel Flow" M1: groove.res / groove.ev aspects
// (twGrooveBuildAspectPayloads, tw/sidecar/twgrooveaspect.h). AC 3's ctest
// gates: encode->store->load->decode round trip byte-identical, byte-
// determinism across two runs, a stale aspectVersion orphans on sight —
// mirroring section4_store's preview-v1 gate above.
// ===========================================================================

// A short, deterministic click train — 6 Hz-ish decaying-sine bursts every
// 0.5 s over 4 s at 48 kHz mono. Not a perceptual fixture (tests/groove/*.wav
// plus groove_test.cc own that job); this one exists only to give the
// front end/pendulum enough periodic structure to recover a tatum
// deterministically, so the encoder's payloads are non-trivial rather than
// exercising only the honest-empty path.
static std::vector<float> grooveClickTrain( uint32_t rate, double totalSec,
                                            double periodSec ) {
    constexpr double kPi      = 3.14159265358979323846;
    constexpr double freqHz   = 200.0;
    constexpr double decaySec = 0.05;
    const uint64_t   n        = (uint64_t)( totalSec * rate );
    std::vector<float> buf( n, 0.0f );
    for ( double t0 = 0.0; t0 < totalSec; t0 += periodSec ) {
        const uint64_t start = (uint64_t)( t0 * rate );
        const uint64_t burstLen = (uint64_t)( decaySec * 6.0 * rate );
        for ( uint64_t i = 0; i < burstLen && start + i < n; i++ ) {
            const double t = (double)i / (double)rate;
            buf[start + i] += (float)( std::sin( 2.0 * kPi * freqHz * t ) *
                                       std::exp( -t / decaySec ) );
        }
    }
    return buf;
}

static void section7_groove( const fs::path &pidDir ) {
    std::cout << "== Section 7: groove.res / groove.ev aspects (proposal 40 M1) ==\n";

    const uint32_t     rate = 48000;
    std::vector<float> mono = grooveClickTrain( rate, 4.0, 0.5 );
    const float        *chans[1] = { mono.data() };

    twGrooveAnalysisParams params;   // defaults
    std::vector<uint8_t>   paramsBlob;
    params.serialize( paramsBlob );
    CHECK( !paramsBlob.empty(), "groove params blob is non-empty" );

    twGrooveAspectPayloads built =
        twGrooveBuildAspectPayloads( chans, 1, mono.size(), rate, params );
    CHECK( built.nUnits > 0,
          "AC3: a periodic click train recovers a lock (nUnits>0, not the honest-empty path)" );
    CHECK( built.resRecordCount > 0, "AC3: groove.res has records" );
    CHECK( built.resPayload.size() ==
              built.resRecordCount * (uint64_t)( built.nUnits + 1 ) * 4,
          "AC3: groove.res payload size == recordCount*(nUnits+1)*4" );
    CHECK( built.evPayload.size() == built.evRecordCount * 20,
          "AC3: groove.ev payload size == recordCount*20" );

    // --------------------------------------------------------- determinism
    {
        twGrooveAspectPayloads built2 =
            twGrooveBuildAspectPayloads( chans, 1, mono.size(), rate, params );
        CHECK( built2.resPayload == built.resPayload,
              "AC3: groove.res bytes are byte-deterministic across two runs" );
        CHECK( built2.evPayload == built.evPayload,
              "AC3: groove.ev bytes are byte-deterministic across two runs" );
        std::vector<uint8_t> paramsBlob2;
        params.serialize( paramsBlob2 );
        CHECK( paramsBlob2 == paramsBlob, "AC2: params blob serialize() is deterministic" );
    }

    // ---------------------------------------------- AC2: two representative
    // params move hashParams (front-end AND pendulum fields).
    {
        twGrooveAnalysisParams p2 = params;
        p2.frontEnd.nBands += 1;
        std::vector<uint8_t> blob2;
        p2.serialize( blob2 );
        CHECK( blob2 != paramsBlob, "AC2: frontEnd.nBands is key material (blob differs)" );
        CHECK( twSidecarStore::hashParams( blob2.data(), blob2.size() ) !=
                  twSidecarStore::hashParams( paramsBlob.data(), paramsBlob.size() ),
              "AC2: frontEnd.nBands changes hashParams" );

        twGrooveAnalysisParams p3 = params;
        p3.pendulum.confidenceFloor += 0.01;
        std::vector<uint8_t> blob3;
        p3.serialize( blob3 );
        CHECK( blob3 != paramsBlob, "AC2: pendulum.confidenceFloor is key material (blob differs)" );
        CHECK( twSidecarStore::hashParams( blob3.data(), blob3.size() ) !=
                  twSidecarStore::hashParams( paramsBlob.data(), paramsBlob.size() ),
              "AC2: pendulum.confidenceFloor changes hashParams" );
    }

    // ------------------------------------------------------ store round trip
    fs::path root = pidDir / "groove";
    g_cleanup.add( root );
    twSidecarStore store;
    store.setRoot( root.string() );
    CHECK( store.enabled(), "AC3: groove store enabled after setRoot" );

    const twContentHash content = hashOfInt( 0x67005Eu );
    const uint64_t      ph =
        twSidecarStore::hashParams( paramsBlob.data(), paramsBlob.size() );

    twQafInfo resInfo;
    resInfo.aspectId      = twAspect::GrooveRes;
    resInfo.aspectVersion = twAspect::GrooveResVersion;
    resInfo.contentHash   = content;
    resInfo.sourceRate    = rate;
    resInfo.channels      = 1;
    resInfo.sourceFrames  = mono.size();
    resInfo.recordStride  = (uint64_t)( built.nUnits + 1 ) * 4;
    resInfo.recordCount   = built.resRecordCount;
    resInfo.hopFrames     = built.hopFrames;
    resInfo.params        = paramsBlob;
    CHECK( store.store( resInfo, built.resPayload.data(), built.resPayload.size() ),
          "AC3: groove.res store() succeeds" );

    twQafInfo evInfo    = resInfo;
    evInfo.aspectId      = twAspect::GrooveEv;
    evInfo.aspectVersion = twAspect::GrooveEvVersion;
    evInfo.recordStride  = 20;
    evInfo.recordCount   = built.evRecordCount;
    evInfo.hopFrames     = 0;
    CHECK( store.store( evInfo, built.evPayload.data(), built.evPayload.size() ),
          "AC3: groove.ev store() succeeds" );

    // load -> decode -> compare byte-identical, then a structural decode check.
    {
        auto rd = store.load( content, twAspect::GrooveRes,
                              twAspect::GrooveResVersion, ph );
        CHECK( rd && rd->isOpen(), "AC3: groove.res load() hit" );
        if ( rd ) {
            std::vector<uint8_t> got;
            CHECK( rd->readAllPayload( got ), "AC3: groove.res readAllPayload succeeds" );
            CHECK( got == built.resPayload, "AC3: groove.res round-trip byte-identical" );
            std::vector<twGrooveResRecord> decoded =
                twGrooveDecodeResPayload( got.data(), got.size(), built.nUnits );
            CHECK( decoded.size() == built.resRecordCount,
                  "AC3: groove.res decode record count matches encoder" );
            for ( const twGrooveResRecord &r : decoded ) {
                for ( float p : r.unitPower )
                    CHECK( p >= 0.0f && p <= 1.0f, "AC1: per-unit resonance power in [0,1]" );
                CHECK( r.compliance >= 0.0f && r.compliance <= 1.0f,
                      "AC1: compliance scalar in [0,1]" );
            }
        }
    }
    {
        auto rd = store.load( content, twAspect::GrooveEv,
                              twAspect::GrooveEvVersion, ph );
        CHECK( rd && rd->isOpen(), "AC3: groove.ev load() hit" );
        if ( rd ) {
            std::vector<uint8_t> got;
            CHECK( rd->readAllPayload( got ), "AC3: groove.ev readAllPayload succeeds" );
            CHECK( got == built.evPayload, "AC3: groove.ev round-trip byte-identical" );
            std::vector<twGrooveEvRecord> decoded =
                twGrooveDecodeEvPayload( got.data(), got.size() );
            CHECK( decoded.size() == built.evRecordCount,
                  "AC3: groove.ev decode record count matches encoder" );
            uint64_t lastPos = 0;
            bool     first   = true;
            for ( const twGrooveEvRecord &e : decoded ) {
                if ( !first )
                    CHECK( e.pos >= lastPos, "AC1: groove.ev records ascending by pos" );
                first   = false;
                lastPos = e.pos;
            }
        }
    }

    // ------------------------------------------------------- version orphan
    // Mirrors section4_store's preview-v1 gate above: a file with a stale
    // aspectVersion is deleted on sight (MISS + orphan), never adopted.
    {
        fs::path onDisk = store.pathFor( content, twAspect::GrooveRes, ph );
        CHECK( fs::exists( onDisk ), "version-orphan precondition: groove.res file present" );
        auto rd = store.load( content, twAspect::GrooveRes,
                              twAspect::GrooveResVersion + 1, ph );
        CHECK( rd == nullptr, "AC3: groove.res MISSES on a stale aspectVersion" );
        CHECK( !fs::exists( onDisk ), "AC3: ...and is orphaned (deleted) on sight" );

        CHECK( store.store( resInfo, built.resPayload.data(), built.resPayload.size() ),
              "AC3: re-store after orphaning succeeds" );
        CHECK( fs::exists( onDisk ), "AC3: file exists again after re-store" );
    }
    {
        fs::path onDisk = store.pathFor( content, twAspect::GrooveEv, ph );
        CHECK( fs::exists( onDisk ), "version-orphan precondition: groove.ev file present" );
        auto rd = store.load( content, twAspect::GrooveEv,
                              twAspect::GrooveEvVersion + 1, ph );
        CHECK( rd == nullptr, "AC3: groove.ev MISSES on a stale aspectVersion" );
        CHECK( !fs::exists( onDisk ), "AC3: ...and is orphaned (deleted) on sight" );
    }
}

// ===========================================================================
// main
// ===========================================================================
int main() {
    std::cout << "sidecar_test starting\n";

    fs::path pidDir = fs::temp_directory_path() /
                      ( "smaragd_sidecar_test_" + std::to_string( selfPid() ) );
    std::error_code ec;
    fs::remove_all( pidDir, ec ); // fresh start (rule: remove_all at start)
    fs::create_directories( pidDir, ec );
    g_cleanup.add( pidDir ); // and remove_all at end (RAII)

    section1_hash();
    fs::path roundtrip = section2_roundtrip();
    section3_validation( roundtrip );
    section4_store( pidDir );
    section5_eviction( pidDir );
    section6_pathFor( pidDir );
    section7_groove( pidDir );

    if ( g_fails == 0 )
        std::cout << "\nAll sidecar tests passed.\n";
    else
        std::cout << "\n" << g_fails << " sidecar test check(s) FAILED.\n";

    return g_fails;
}
