#include "tw/core/twlog.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <thread>

namespace tw {

// ---------------------------------------------------------------------------
// Per-thread state.
//
// t_nonBlocking: set once by the realtime audio callback (markNonBlocking).
// Such a thread truncates at TW_LOG_RT_MAX and takes the ring slot with
// try_lock, counting a drop rather than waiting — invariant 4, proposal 24 §2.1.
// t_slot: this thread's interned id, assigned lazily on its first record.
//
// EVERY thread_local here is a POD, and that is load-bearing, not incidental.
// On this toolchain (MinGW-w64 GCC 13.1.0, x86_64-posix-seh) a `thread_local`
// with a NON-trivial destructor — std::string, std::vector, anything needing
// __cxa_thread_atexit — corrupts the heap once ~3+ threads touch it. Verified
// in isolation: an 8-thread program whose only shared state is a
// `thread_local std::vector<char>` dies with STATUS_HEAP_CORRUPTION (0xc0000374)
// 10 runs out of 10, with none of this file's code linked in. Message
// formatting therefore uses a stack buffer plus a rare function-local heap
// fallback (see vlogf) rather than a cached per-thread buffer.
// ---------------------------------------------------------------------------
namespace {

thread_local bool        t_nonBlocking = false;
thread_local uint32_t    t_slot        = UINT32_MAX;   // unassigned
thread_local const char *t_pendingName = nullptr;

// Messages at or under this length format without touching the heap. Chosen
// well above the realistic maximum: the longest existing diagnostic in the tree
// is a couple of hundred characters.
constexpr size_t kStackFmt = 1024;

}  // namespace

// ---------------------------------------------------------------------------

struct TwLog::Impl
{
    mutable std::mutex      mtx;

    // The ring. slot = seq % ring.size(); nextSeq is one past the newest.
    std::vector<LogRecord>  ring;
    uint64_t                nextSeq = 0;

    std::atomic<int>        minLevel{ static_cast<int>( LogLevel::Debug ) };
    std::atomic<bool>       console{ false };
    std::atomic<uint64_t>   dropped{ 0 };

    // Interned categories. catPtrs is a pointer-identity fast path: nearly every
    // call site passes a string literal, so the same pointer recurs and we never
    // reach the strcmp scan.
    std::vector<const char *> catPtrs;
    std::vector<uint16_t>     catPtrIds;
    std::vector<std::string>  catNames;

    std::vector<std::string>  threadNames;

    // Clock. tMonoUs is monotonic (ordering, deltas); wall-clock rendering adds
    // startWallMs + localOffsetMs, both computed once so no record pays for a
    // timezone lookup.
    std::chrono::steady_clock::time_point startMono;
    int64_t                 startWallMs   = 0;
    int64_t                 localOffsetMs = 0;

    // Rotating file sink, drained by writerThread via the same snapshot() the
    // UI uses — it holds a cursor, so there is no second queue to keep coherent.
    std::thread             writerThread;
    std::condition_variable writerCv;
    std::mutex              writerMtx;
    bool                    writerStop = false;
    std::string             fileDir;
    size_t                  fileMaxBytes = 0;
    int                     fileKeep     = 0;
    FILE                   *fp           = nullptr;
    size_t                  fpBytes      = 0;
    uint64_t                fileCursor   = 0;

    void  appendLocked( LogLevel lvl, uint16_t catId, uint32_t threadId,
                        const char *text, size_t len );
    void  emit( LogLevel lvl, const char *cat, const char *text, size_t len );
    uint16_t internCategoryLocked( const char *cat );
    uint32_t threadSlotLocked();
    // Caller must hold mtx. The public TwLog::formatLine is the locking wrapper;
    // appendLocked needs this one because mtx is not recursive.
    std::string formatLineLocked( const LogRecord &rec ) const;

    void  writerLoop();
    void  openFile();
    void  rotateFile();
    void  closeFile();
};

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

TwLog::TwLog()
    : d_( new Impl )
{
    d_->startMono = std::chrono::steady_clock::now();

    using namespace std::chrono;
    d_->startWallMs = duration_cast<milliseconds>(
        system_clock::now().time_since_epoch() ).count();

    // Resolve the UTC->local offset ONCE, here, while we are still
    // single-threaded. Per-record formatting is then pure arithmetic — no
    // localtime call, hence no thread-safety question and no per-line cost.
    {
        std::time_t now = std::time( nullptr );
        std::tm gm{}, lt{};
#if defined( _WIN32 )
        gmtime_s( &gm, &now );
        localtime_s( &lt, &now );
#else
        gmtime_r( &now, &gm );
        localtime_r( &now, &lt );
#endif
        gm.tm_isdst = lt.tm_isdst;
        const std::time_t gmBack = std::mktime( &gm );
        const std::time_t ltBack = std::mktime( &lt );
        d_->localOffsetMs = static_cast<int64_t>( gmBack - ltBack ) * 1000;
    }

    setCapacity( 200000 );
}

TwLog::~TwLog()
{
    shutdown();
    delete d_;
}

TwLog &TwLog::instance()
{
    static TwLog inst;
    return inst;
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

void TwLog::setCapacity( size_t records )
{
    if( records < 16 ) records = 16;
    std::lock_guard<std::mutex> lk( d_->mtx );
    d_->ring.clear();
    d_->ring.resize( records );
    d_->nextSeq = 0;

    // Note: slots are NOT pre-reserved. Reserving TW_LOG_RT_MAX per slot would
    // cost a 200k-record ring ~64 MB the moment it is configured, whether or not
    // anything is ever logged. Letting each slot's string grow to its own
    // largest message instead makes the cost proportional to what actually gets
    // logged, and still reaches the same steady state: after a slot's first
    // write, assign() reuses that capacity and never allocates again.
}

size_t TwLog::capacity() const
{
    std::lock_guard<std::mutex> lk( d_->mtx );
    return d_->ring.size();
}

void TwLog::setConsole( bool on )   { d_->console.store( on, std::memory_order_relaxed ); }
bool TwLog::console() const         { return d_->console.load( std::memory_order_relaxed ); }

void TwLog::setMinLevel( LogLevel lvl )
{
    d_->minLevel.store( static_cast<int>( lvl ), std::memory_order_relaxed );
}

LogLevel TwLog::minLevel() const
{
    return static_cast<LogLevel>( d_->minLevel.load( std::memory_order_relaxed ) );
}

void TwLog::setFileSink( const std::string &dir, size_t maxBytes, int keep )
{
    shutdown();   // idempotent; tears down any previous writer

    {
        std::lock_guard<std::mutex> lk( d_->mtx );
        d_->fileDir      = dir;
        d_->fileMaxBytes = maxBytes ? maxBytes : ( 8u * 1024u * 1024u );
        d_->fileKeep     = keep < 0 ? 0 : keep;
        // Start from the oldest record still resident, so everything logged
        // before the config directory was known still reaches the file.
        d_->fileCursor   = ( d_->nextSeq > d_->ring.size() )
                             ? d_->nextSeq - d_->ring.size() : 0;
    }

    {
        std::lock_guard<std::mutex> lk( d_->writerMtx );
        d_->writerStop = false;
    }
    d_->writerThread = std::thread( [this] { d_->writerLoop(); } );
}

void TwLog::shutdown()
{
    if( d_->writerThread.joinable() ) {
        {
            std::lock_guard<std::mutex> lk( d_->writerMtx );
            d_->writerStop = true;
        }
        d_->writerCv.notify_all();
        d_->writerThread.join();
    }
    d_->closeFile();
}

// ---------------------------------------------------------------------------
// Interning
// ---------------------------------------------------------------------------

uint16_t TwLog::Impl::internCategoryLocked( const char *cat )
{
    if( !cat ) cat = "";

    // Fast path: pointer identity. Call sites pass literals, so this hits.
    //
    // The strcmp confirmation is NOT redundant. A caller may pass a temporary
    // (a std::string's c_str(), say); once that string dies its address can be
    // handed back by a later allocation, and a bare pointer match would then
    // silently file records under the wrong category. Confirming costs one
    // compare of a ~10-byte string and makes the cache sound for any argument.
    for( size_t i = 0; i < catPtrs.size(); ++i ) {
        if( catPtrs[i] == cat ) {
            const uint16_t id = catPtrIds[i];
            if( id < catNames.size() && catNames[id] == cat ) return id;
            // Address reused by different text — drop the stale entry and fall
            // through to the authoritative scan below.
            catPtrs.erase( catPtrs.begin() + i );
            catPtrIds.erase( catPtrIds.begin() + i );
            break;
        }
    }

    // Slow path: same text through a different pointer.
    uint16_t id = 0;
    bool found = false;
    for( size_t i = 0; i < catNames.size(); ++i ) {
        if( catNames[i] == cat ) { id = static_cast<uint16_t>( i ); found = true; break; }
    }
    if( !found ) {
        if( catNames.size() >= 0xFFFEu ) return 0;   // pathological; bucket it
        id = static_cast<uint16_t>( catNames.size() );
        catNames.emplace_back( cat );
    }

    catPtrs.push_back( cat );
    catPtrIds.push_back( id );
    return id;
}

uint32_t TwLog::Impl::threadSlotLocked()
{
    if( t_slot == UINT32_MAX ) {
        t_slot = static_cast<uint32_t>( threadNames.size() );
        if( t_pendingName && *t_pendingName ) {
            threadNames.emplace_back( t_pendingName );
        } else {
            char buf[32];
            std::snprintf( buf, sizeof buf, "thread-%u", (unsigned)t_slot );
            threadNames.emplace_back( buf );
        }
        t_pendingName = nullptr;
    } else if( t_pendingName && *t_pendingName ) {
        // Renamed after its first record.
        if( t_slot < threadNames.size() ) threadNames[t_slot] = t_pendingName;
        t_pendingName = nullptr;
    }
    return t_slot;
}

void TwLog::markNonBlocking() { t_nonBlocking = true; }
bool TwLog::nonBlocking()     { return t_nonBlocking; }

void TwLog::nameThread( const char *name )
{
    t_pendingName = name;
}

// ---------------------------------------------------------------------------
// Emit
// ---------------------------------------------------------------------------

void TwLog::Impl::appendLocked( LogLevel lvl, uint16_t catId, uint32_t threadId,
                                const char *text, size_t len )
{
    if( ring.empty() ) return;

    LogRecord &r = ring[ nextSeq % ring.size() ];
    r.seq      = nextSeq;
    r.level    = lvl;
    r.catId    = catId;
    r.threadId = threadId;
    r.tMonoUs  = std::chrono::duration_cast<std::chrono::microseconds>(
                     std::chrono::steady_clock::now() - startMono ).count();
    r.text.assign( text, len );      // reuses the slot's capacity
    ++nextSeq;

    if( console.load( std::memory_order_relaxed ) ) {
        const std::string line = formatLineLocked( r );   // mtx is held here
        std::fwrite( line.data(), 1, line.size(), stderr );
        std::fputc( '\n', stderr );
        std::fflush( stderr );
    }
}

// Split `text` into lines and append each as its own record (invariant 3), all
// under ONE lock acquisition so a multi-line message stays contiguous.
void TwLog::Impl::emit( LogLevel lvl, const char *cat, const char *text, size_t len )
{
    // Trim trailing newlines — swept fprintf sites and the Qt bridge both carry
    // them, and a record is a line, not a line plus a terminator.
    while( len > 0 && ( text[len - 1] == '\n' || text[len - 1] == '\r' ) ) --len;

    if( t_nonBlocking ) {
        std::unique_lock<std::mutex> lk( mtx, std::try_to_lock );
        if( !lk.owns_lock() ) {
            dropped.fetch_add( 1, std::memory_order_relaxed );
            return;
        }
        const uint16_t catId    = internCategoryLocked( cat );
        const uint32_t threadId = threadSlotLocked();
        size_t start = 0;
        for( size_t i = 0; i <= len; ++i ) {
            if( i == len || text[i] == '\n' ) {
                appendLocked( lvl, catId, threadId, text + start, i - start );
                start = i + 1;
            }
        }
        return;
    }

    std::lock_guard<std::mutex> lk( mtx );
    const uint16_t catId    = internCategoryLocked( cat );
    const uint32_t threadId = threadSlotLocked();
    size_t start = 0;
    for( size_t i = 0; i <= len; ++i ) {
        if( i == len || text[i] == '\n' ) {
            appendLocked( lvl, catId, threadId, text + start, i - start );
            start = i + 1;
        }
    }
}

void TwLog::logf( LogLevel lvl, const char *cat, const char *file, int line,
                  const char *fmt, ... )
{
    va_list ap;
    va_start( ap, fmt );
    vlogf( lvl, cat, file, line, fmt, ap );
    va_end( ap );
}

void TwLog::vlogf( LogLevel lvl, const char *cat, const char *file, int line,
                   const char *fmt, va_list ap )
{
    (void)file; (void)line;
    if( static_cast<int>( lvl ) > d_->minLevel.load( std::memory_order_relaxed ) )
        return;
    if( !fmt ) return;

    if( t_nonBlocking ) {
        // Realtime path: stack buffer, no allocation ever, truncating.
        char buf[TW_LOG_RT_MAX];
        const int n = std::vsnprintf( buf, sizeof buf, fmt, ap );
        const size_t len = ( n < 0 ) ? 0
                         : ( static_cast<size_t>( n ) >= sizeof buf
                               ? sizeof buf - 1 : static_cast<size_t>( n ) );
        d_->emit( lvl, cat, buf, len );
        return;
    }

    // Format once into a stack buffer; that covers every message this codebase
    // actually emits. Only a >1 KB message pays for a heap buffer, and it is a
    // plain function-local vector — never a cached thread_local (see the note
    // on the toolchain's thread_local defect above).
    char buf[kStackFmt];
    va_list ap2;
    va_copy( ap2, ap );
    const int n = std::vsnprintf( buf, sizeof buf, fmt, ap );
    if( n < 0 ) { va_end( ap2 ); return; }

    if( static_cast<size_t>( n ) < sizeof buf ) {
        va_end( ap2 );
        d_->emit( lvl, cat, buf, static_cast<size_t>( n ) );
        return;
    }

    std::vector<char> big( static_cast<size_t>( n ) + 1 );
    std::vsnprintf( big.data(), big.size(), fmt, ap2 );
    va_end( ap2 );
    d_->emit( lvl, cat, big.data(), static_cast<size_t>( n ) );
}

void TwLog::logStr( LogLevel lvl, const char *cat, const char *file, int line,
                    const std::string &msg )
{
    (void)file; (void)line;
    if( static_cast<int>( lvl ) > d_->minLevel.load( std::memory_order_relaxed ) )
        return;
    d_->emit( lvl, cat, msg.data(), msg.size() );
}

// ---------------------------------------------------------------------------
// Reader side
// ---------------------------------------------------------------------------

uint64_t TwLog::firstSeq() const
{
    std::lock_guard<std::mutex> lk( d_->mtx );
    return ( d_->nextSeq > d_->ring.size() ) ? d_->nextSeq - d_->ring.size() : 0;
}

uint64_t TwLog::nextSeq() const
{
    std::lock_guard<std::mutex> lk( d_->mtx );
    return d_->nextSeq;
}

size_t TwLog::snapshot( uint64_t from, uint64_t to, std::vector<LogRecord> &out ) const
{
    out.clear();
    std::lock_guard<std::mutex> lk( d_->mtx );
    if( d_->ring.empty() ) return 0;

    const uint64_t first = ( d_->nextSeq > d_->ring.size() )
                             ? d_->nextSeq - d_->ring.size() : 0;
    if( from < first )       from = first;
    if( to   > d_->nextSeq ) to   = d_->nextSeq;
    if( from >= to ) return 0;

    out.reserve( static_cast<size_t>( to - from ) );
    for( uint64_t s = from; s < to; ++s ) out.push_back( d_->ring[ s % d_->ring.size() ] );
    return out.size();
}

uint64_t TwLog::droppedCount() const
{
    return d_->dropped.load( std::memory_order_relaxed );
}

const char *TwLog::categoryName( uint16_t catId )
{
    Impl *d = instance().d_;
    std::lock_guard<std::mutex> lk( d->mtx );
    return catId < d->catNames.size() ? d->catNames[catId].c_str() : "";
}

uint16_t TwLog::categoryCount()
{
    Impl *d = instance().d_;
    std::lock_guard<std::mutex> lk( d->mtx );
    return static_cast<uint16_t>( d->catNames.size() );
}

const char *TwLog::threadName( uint32_t threadId )
{
    Impl *d = instance().d_;
    std::lock_guard<std::mutex> lk( d->mtx );
    return threadId < d->threadNames.size() ? d->threadNames[threadId].c_str() : "";
}

uint32_t TwLog::threadCount()
{
    Impl *d = instance().d_;
    std::lock_guard<std::mutex> lk( d->mtx );
    return static_cast<uint32_t>( d->threadNames.size() );
}

const char *TwLog::levelName( LogLevel lvl )
{
    switch( lvl ) {
        case LogLevel::Error: return "ERR";
        case LogLevel::Warn:  return "WRN";
        case LogLevel::Info:  return "INF";
        case LogLevel::Debug: return "DBG";
        case LogLevel::Trace: return "TRC";
    }
    return "???";
}

std::string TwLog::formatLine( const LogRecord &rec )
{
    Impl *d = instance().d_;
    std::lock_guard<std::mutex> lk( d->mtx );
    return d->formatLineLocked( rec );
}

void TwLog::formatTimestamp( const LogRecord &rec, char *out, size_t cap )
{
    if( !out || cap == 0 ) return;
    Impl *d = instance().d_;
    // No lock: startWallMs and localOffsetMs are written once, in the
    // constructor, before any other thread can observe this object.
    const int64_t wallMs = d->startWallMs + rec.tMonoUs / 1000 - d->localOffsetMs;

    int64_t secOfDay = ( wallMs / 1000 ) % 86400;
    if( secOfDay < 0 ) secOfDay += 86400;
    int ms = static_cast<int>( wallMs % 1000 );
    if( ms < 0 ) ms += 1000;

    std::snprintf( out, cap, "%02d:%02d:%02d.%03d",
                   static_cast<int>( secOfDay / 3600 ),
                   static_cast<int>( ( secOfDay / 60 ) % 60 ),
                   static_cast<int>( secOfDay % 60 ), ms );
}

std::string TwLog::Impl::formatLineLocked( const LogRecord &rec ) const
{
    const char *cat = rec.catId < catNames.size() ? catNames[rec.catId].c_str() : "";

    char stamp[16];
    TwLog::formatTimestamp( rec, stamp, sizeof stamp );

    char head[96];
    std::snprintf( head, sizeof head, "%s [%s] %-10s ",
                   stamp, TwLog::levelName( rec.level ), cat );

    std::string out( head );
    out += rec.text;
    return out;
}

// ---------------------------------------------------------------------------
// Rotating file sink
// ---------------------------------------------------------------------------

void TwLog::Impl::openFile()
{
    if( fp ) return;
    const std::string path = fileDir + "/smaragd.log";
    fp = std::fopen( path.c_str(), "ab" );
    if( !fp ) return;
    std::fseek( fp, 0, SEEK_END );
    const long pos = std::ftell( fp );
    fpBytes = pos > 0 ? static_cast<size_t>( pos ) : 0;
}

void TwLog::Impl::closeFile()
{
    if( fp ) { std::fflush( fp ); std::fclose( fp ); fp = nullptr; }
    fpBytes = 0;
}

void TwLog::Impl::rotateFile()
{
    closeFile();
    const std::string base = fileDir + "/smaragd.log";
    if( fileKeep > 0 ) {
        std::remove( ( base + "." + std::to_string( fileKeep ) ).c_str() );
        for( int i = fileKeep - 1; i >= 1; --i ) {
            std::rename( ( base + "." + std::to_string( i ) ).c_str(),
                         ( base + "." + std::to_string( i + 1 ) ).c_str() );
        }
        std::rename( base.c_str(), ( base + ".1" ).c_str() );
    } else {
        std::remove( base.c_str() );
    }
    openFile();
}

void TwLog::Impl::writerLoop()
{
    TwLog::nameThread( "log-writer" );
    openFile();

    std::vector<LogRecord> batch;
    for(;;) {
        bool stopping;
        {
            std::unique_lock<std::mutex> lk( writerMtx );
            writerCv.wait_for( lk, std::chrono::milliseconds( 250 ),
                               [this] { return writerStop; } );
            stopping = writerStop;
        }

        for(;;) {
            uint64_t to;
            {
                std::lock_guard<std::mutex> lk( mtx );
                to = nextSeq;
                // A gap means the ring lapped us; say so rather than losing it
                // silently.
                const uint64_t first = ( nextSeq > ring.size() ) ? nextSeq - ring.size() : 0;
                if( fileCursor < first ) {
                    if( fp ) {
                        char note[96];
                        const int n = std::snprintf(
                            note, sizeof note,
                            "--- %llu records dropped (file writer fell behind) ---\n",
                            (unsigned long long)( first - fileCursor ) );
                        std::fwrite( note, 1, (size_t)n, fp );
                        fpBytes += (size_t)n;
                    }
                    fileCursor = first;
                }
            }
            if( fileCursor >= to ) break;

            uint64_t chunkEnd = to;
            if( chunkEnd - fileCursor > 8192 ) chunkEnd = fileCursor + 8192;
            TwLog::instance().snapshot( fileCursor, chunkEnd, batch );
            if( batch.empty() ) break;

            for( const LogRecord &r : batch ) {
                if( !fp ) break;
                const std::string line = TwLog::formatLine( r );
                std::fwrite( line.data(), 1, line.size(), fp );
                std::fputc( '\n', fp );
                fpBytes += line.size() + 1;
                if( fpBytes >= fileMaxBytes ) rotateFile();
            }
            fileCursor = batch.back().seq + 1;
        }

        if( fp ) std::fflush( fp );
        if( stopping ) break;
    }

    closeFile();
}

}  // namespace tw
