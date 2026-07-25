#include "tw/sidecar/twsidecarstore.h"

#include "tw/core/twlog.h"

#include <algorithm>
#include <cstdio>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

twSidecarStore &twSidecarStore::instance()
{
    // IMMORTAL singleton (never destroyed): revalidator workers may touch the
    // store arbitrarily late in process life, and a function-local static by
    // value is destroyed during static destruction while such a worker can
    // still be running — the M2 teardown segfault (worker read a destroyed
    // root_ path inside buildPath). Leaking one small object at exit is the
    // standard and safe answer; the OS reclaims it. Orderly teardown (joining
    // workers before exit) is ALSO enforced at the app layer, but the store
    // must not be the thing that crashes when some future caller gets that
    // ordering wrong.
    static twSidecarStore *s = new twSidecarStore();
    return *s;
}

void twSidecarStore::setRoot( const std::string &utf8Path )
{
    std::lock_guard<std::mutex> lock( mutex_ );

    if( utf8Path.empty() ) {
        root_.clear();
        enabled_ = false;
        return;
    }

    fs::path p = fs::u8path( utf8Path );
    std::error_code ec;
    fs::create_directories( p, ec );
    if( ec ) {
        root_.clear();
        enabled_ = false;
        TW_LOGW( "sidecar", "setRoot: cannot create '%s' (%s); store disabled",
                 utf8Path.c_str(), ec.message().c_str() );
        return;
    }

    root_    = std::move( p );
    enabled_ = true;
}

bool twSidecarStore::enabled() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return enabled_;
}

void twSidecarStore::setSizeCapBytes( uint64_t cap )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    sizeCap_ = cap;
}

uint64_t twSidecarStore::sizeCapBytes() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return sizeCap_;
}

uint64_t twSidecarStore::hashParams( const void *params, size_t len )
{
    return twHashBuffer( params, len ).lo;
}

// Build the on-disk path for a key. Caller holds mutex_ (or does not need to,
// as it only reads root_); the public pathFor() takes the lock.
static fs::path buildPath( const fs::path &root, const twContentHash &content,
                           const std::string &aspectId, uint64_t paramsHash )
{
    const std::string hex = content.toHex();          // 32 lowercase hex chars
    const std::string hh  = hex.substr( 0, 2 );       // shard directory

    char pbuf[17];
    std::snprintf( pbuf, sizeof( pbuf ), "%016llx", (unsigned long long)paramsHash );

    std::string name = hex + "." + aspectId + "." + pbuf + ".qaf";
    return root / hh / name;
}

// Enforce the size cap over `root`. Caller holds mutex_. Never throws, never
// fails the caller: filesystem errors are swallowed via std::error_code.
static void evictUnderCap( const fs::path &root, uint64_t cap )
{
    struct Entry {
        fs::path           path;
        uint64_t           size = 0;
        fs::file_time_type mtime;
    };

    std::vector<Entry> files;
    uint64_t total = 0;

    std::error_code ec;
    fs::recursive_directory_iterator it( root, ec );
    if( ec )
        return;    // cannot scan; never fail the caller
    const fs::recursive_directory_iterator end;

    for( ; it != end; it.increment( ec ) ) {
        if( ec )
            break;
        const fs::directory_entry &de = *it;

        std::error_code e2;
        if( !de.is_regular_file( e2 ) || e2 )
            continue;
        if( de.path().extension() != ".qaf" )
            continue;

        Entry entry;
        entry.path = de.path();
        entry.size = (uint64_t)fs::file_size( de.path(), e2 );
        if( e2 )
            continue;
        entry.mtime = fs::last_write_time( de.path(), e2 );
        if( e2 )
            continue;

        total += entry.size;
        files.push_back( std::move( entry ) );
    }

    if( total <= cap )
        return;

    // LRU: oldest mtime first.
    std::sort( files.begin(), files.end(),
               []( const Entry &a, const Entry &b ) { return a.mtime < b.mtime; } );

    for( const Entry &e : files ) {
        if( total <= cap )
            break;

        std::error_code de;
        const bool removed = fs::remove( e.path, de );
        if( de || !removed ) {
            // A locked file (open handle / mapped view) is EXPECTED on Windows;
            // skip it and let a later pass retry. Never a warning.
            TW_LOGD( "sidecar", "evict: skipping undeletable '%s'",
                     e.path.u8string().c_str() );
            continue;
        }
        total -= e.size;
    }
}

fs::path twSidecarStore::pathFor( const twContentHash &content,
                                  const std::string &aspectId,
                                  uint64_t paramsHash ) const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return buildPath( root_, content, aspectId, paramsHash );
}

std::unique_ptr<twQafReader> twSidecarStore::load( const twContentHash &content,
                                                   const std::string &aspectId,
                                                   uint32_t aspectVersion,
                                                   uint64_t paramsHash )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    if( !enabled_ )
        return nullptr;

    const fs::path path = buildPath( root_, content, aspectId, paramsHash );

    auto reader = std::make_unique<twQafReader>();
    if( !reader->open( path ) )
        return nullptr;

    const twQafInfo &info = reader->info();

    // Identity match is total: aspect id + aspect version + content hash +
    // params hash. A mismatched aspectVersion is orphaned (deleted on sight).
    if( info.aspectId != aspectId )
        return nullptr;

    if( info.aspectVersion != aspectVersion ) {
        reader->close();
        std::error_code ec;
        fs::remove( path, ec );        // version orphaning; ignore failure
        return nullptr;
    }

    if( info.contentHash != content )
        return nullptr;

    if( hashParams( info.params.data(), info.params.size() ) != paramsHash )
        return nullptr;

    // Hit: bump mtime (LRU touch) BEFORE handing back the open reader.
    std::error_code ec;
    fs::last_write_time( path, fs::file_time_type::clock::now(), ec );
    // A failed touch is harmless — the file is still a valid hit.

    return reader;
}

bool twSidecarStore::store( const twQafInfo &info, const void *payload,
                            uint64_t payloadLen )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    if( !enabled_ )
        return false;

    const uint64_t paramsHash = hashParams( info.params.data(), info.params.size() );
    const fs::path path = buildPath( root_, info.contentHash, info.aspectId, paramsHash );

    if( !twQafWriter::write( path, info, payload, payloadLen ) )
        return false;

    // Enforce the size cap (already holding the lock).
    evictUnderCap( root_, sizeCap_ );
    return true;
}

void twSidecarStore::evictIfNeeded()
{
    std::lock_guard<std::mutex> lock( mutex_ );
    if( !enabled_ )
        return;
    evictUnderCap( root_, sizeCap_ );
}
