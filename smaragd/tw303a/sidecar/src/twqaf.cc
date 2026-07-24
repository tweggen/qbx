#include "tw/sidecar/twqaf.h"

#include "tw/core/twlog.h"

#include <cstring>
#include <system_error>

#ifdef _WIN32
#  include <io.h>       // _commit
#  define TW_FSYNC( f )   _commit( _fileno( f ) )
#  define TW_FSEEK64( f, off, whence )  _fseeki64( ( f ), ( off ), ( whence ) )
#  define TW_FTELL64( f )              _ftelli64( f )
#else
#  include <unistd.h>   // fsync, fileno
#  define TW_FSYNC( f )   ::fsync( ::fileno( f ) )
#  define TW_FSEEK64( f, off, whence )  ::fseeko( ( f ), ( off ), ( whence ) )
#  define TW_FTELL64( f )              ::ftello( f )
#endif

namespace {

// Fixed header size (offset of the params blob). Every writer emits exactly
// this; every reader validates against it.
constexpr uint64_t kHeaderSize    = 144;
constexpr uint32_t kFormatVersion = 1;

// --- little-endian field encoders ------------------------------------------
// Explicit, field-by-field serialization — never a struct memcpy, so the on-
// disk layout is byte-stable across compilers/padding.

inline void putU32( uint8_t *p, uint32_t v )
{
    p[0] = (uint8_t)( v & 0xff );
    p[1] = (uint8_t)( ( v >> 8 ) & 0xff );
    p[2] = (uint8_t)( ( v >> 16 ) & 0xff );
    p[3] = (uint8_t)( ( v >> 24 ) & 0xff );
}

inline void putU64( uint8_t *p, uint64_t v )
{
    for( int i = 0; i < 8; i++ ) {
        p[i] = (uint8_t)( v & 0xff );
        v >>= 8;
    }
}

inline uint32_t getU32( const uint8_t *p )
{
    return (uint32_t)p[0]
         | ( (uint32_t)p[1] << 8 )
         | ( (uint32_t)p[2] << 16 )
         | ( (uint32_t)p[3] << 24 );
}

inline uint64_t getU64( const uint8_t *p )
{
    uint64_t v = 0;
    for( int i = 7; i >= 0; i-- )
        v = ( v << 8 ) | p[i];
    return v;
}

// CRC-32/ISO-HDLC lookup table (zlib polynomial 0xEDB88320), built on first use.
struct Crc32Table {
    uint32_t t[256];
    Crc32Table()
    {
        for( uint32_t i = 0; i < 256; i++ ) {
            uint32_t c = i;
            for( int k = 0; k < 8; k++ )
                c = ( c & 1 ) ? ( 0xEDB88320u ^ ( c >> 1 ) ) : ( c >> 1 );
            t[i] = c;
        }
    }
};

const Crc32Table &crcTable()
{
    static const Crc32Table tbl;
    return tbl;
}

} // namespace

uint32_t twCrc32( const void *data, size_t len, uint32_t seed )
{
    const uint32_t *t = crcTable().t;
    const uint8_t  *p = static_cast<const uint8_t *>( data );
    uint32_t c = seed ^ 0xffffffffu;
    for( size_t i = 0; i < len; i++ )
        c = t[( c ^ p[i] ) & 0xff] ^ ( c >> 8 );
    return c ^ 0xffffffffu;
}

// ---------------------------------------------------------------------------
// twQafWriter
// ---------------------------------------------------------------------------

bool twQafWriter::write( const std::filesystem::path &path,
                         const twQafInfo &info,
                         const void *payload, uint64_t payloadLen )
{
    namespace fs = std::filesystem;

    const uint64_t paramsLen      = (uint64_t)info.params.size();
    const uint64_t paramsOffset   = kHeaderSize;
    const uint64_t seekTableOffset = 0;   // no variable-stride aspect yet
    const uint64_t seekTableLen   = 0;
    const uint64_t payloadOffset  = kHeaderSize + paramsLen;

    // aspectId: <= 16 ASCII chars, NUL-padded.
    if( info.aspectId.size() > 16 ) {
        TW_LOGW( "sidecar", "twQafWriter::write: aspectId '%s' exceeds 16 chars",
                 info.aspectId.c_str() );
        return false;
    }

    // Assemble the fixed 144-byte header field-by-field.
    uint8_t hdr[kHeaderSize];
    std::memset( hdr, 0, sizeof( hdr ) );

    hdr[0] = 'Q'; hdr[1] = 'A'; hdr[2] = 'F'; hdr[3] = '1';
    putU32( hdr + 4, kFormatVersion );
    std::memcpy( hdr + 8, info.aspectId.data(), info.aspectId.size() ); // NUL-padded
    putU32( hdr + 24, info.aspectVersion );
    putU32( hdr + 28, 0 );                       // flags
    putU64( hdr + 32, info.contentHash.lo );
    putU64( hdr + 40, info.contentHash.hi );
    putU32( hdr + 48, info.sourceRate );
    putU32( hdr + 52, info.channels );
    putU64( hdr + 56, info.sourceFrames );
    putU64( hdr + 64, info.recordStride );
    putU64( hdr + 72, info.recordCount );
    putU64( hdr + 80, info.hopFrames );
    putU64( hdr + 88, paramsOffset );
    putU64( hdr + 96, paramsLen );
    putU64( hdr + 104, seekTableOffset );
    putU64( hdr + 112, seekTableLen );
    putU64( hdr + 120, payloadOffset );
    putU64( hdr + 128, payloadLen );
    putU32( hdr + 136, 0 );                       // reserved
    putU32( hdr + 140, twCrc32( hdr, 140 ) );     // CRC over [0,140)

    // Ensure the parent directory exists.
    std::error_code ec;
    fs::path parent = path.parent_path();
    if( !parent.empty() ) {
        fs::create_directories( parent, ec );
        if( ec ) {
            TW_LOGW( "sidecar", "twQafWriter::write: create_directories failed: %s",
                     ec.message().c_str() );
            return false;
        }
    }

    // Write atomically: "<path>.tmp", flush + fsync, then rename over.
    fs::path tmp = path;
    tmp += ".tmp";

#ifdef _WIN32
    FILE *f = _wfopen( tmp.wstring().c_str(), L"wb" );
#else
    FILE *f = std::fopen( tmp.c_str(), "wb" );
#endif
    if( !f ) {
        TW_LOGW( "sidecar", "twQafWriter::write: cannot open temp for write" );
        return false;
    }

    bool ok = std::fwrite( hdr, 1, sizeof( hdr ), f ) == sizeof( hdr );
    if( ok && paramsLen > 0 )
        ok = std::fwrite( info.params.data(), 1, (size_t)paramsLen, f ) == (size_t)paramsLen;
    if( ok && payloadLen > 0 )
        ok = payload != nullptr
          && std::fwrite( payload, 1, (size_t)payloadLen, f ) == (size_t)payloadLen;

    if( ok )
        ok = std::fflush( f ) == 0;
    if( ok )
        ok = TW_FSYNC( f ) == 0;

    if( std::fclose( f ) != 0 )
        ok = false;

    if( !ok ) {
        TW_LOGW( "sidecar", "twQafWriter::write: write/flush failed; discarding temp" );
        fs::remove( tmp, ec );
        return false;
    }

    fs::rename( tmp, path, ec );
    if( ec ) {
        TW_LOGW( "sidecar", "twQafWriter::write: rename over target failed: %s",
                 ec.message().c_str() );
        fs::remove( tmp, ec );
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// twQafReader
// ---------------------------------------------------------------------------

twQafReader::~twQafReader()
{
    close();
}

void twQafReader::close()
{
    if( f_ ) {
        std::fclose( f_ );
        f_ = nullptr;
    }
}

bool twQafReader::open( const std::filesystem::path &path )
{
    namespace fs = std::filesystem;

    close();

    std::error_code ec;
    const uint64_t fileSize = (uint64_t)fs::file_size( path, ec );
    if( ec )
        return false;
    if( fileSize < kHeaderSize )
        return false;

#ifdef _WIN32
    f_ = _wfopen( path.wstring().c_str(), L"rb" );
#else
    f_ = std::fopen( path.c_str(), "rb" );
#endif
    if( !f_ )
        return false;

    uint8_t hdr[kHeaderSize];
    if( std::fread( hdr, 1, sizeof( hdr ), f_ ) != sizeof( hdr ) ) {
        close();
        return false;
    }

    // magic + format version
    if( hdr[0] != 'Q' || hdr[1] != 'A' || hdr[2] != 'F' || hdr[3] != '1' ) {
        close();
        return false;
    }
    if( getU32( hdr + 4 ) != kFormatVersion ) {
        close();
        return false;
    }

    // CRC over [0,140)
    if( getU32( hdr + 140 ) != twCrc32( hdr, 140 ) ) {
        close();
        return false;
    }

    // Decode fields.
    char aspect[17];
    std::memcpy( aspect, hdr + 8, 16 );
    aspect[16] = '\0';

    twQafInfo info;
    info.aspectId       = std::string( aspect ); // stops at first NUL
    info.aspectVersion  = getU32( hdr + 24 );
    // flags at 28 (reserved, ignored)
    info.contentHash.lo = getU64( hdr + 32 );
    info.contentHash.hi = getU64( hdr + 40 );
    info.sourceRate     = getU32( hdr + 48 );
    info.channels       = getU32( hdr + 52 );
    info.sourceFrames   = getU64( hdr + 56 );
    info.recordStride   = getU64( hdr + 64 );
    info.recordCount    = getU64( hdr + 72 );
    info.hopFrames      = getU64( hdr + 80 );

    const uint64_t paramsOffset    = getU64( hdr + 88 );
    const uint64_t paramsLen       = getU64( hdr + 96 );
    const uint64_t seekTableOffset = getU64( hdr + 104 );
    const uint64_t seekTableLen    = getU64( hdr + 112 );
    const uint64_t payloadOffset   = getU64( hdr + 120 );
    const uint64_t payloadLen      = getU64( hdr + 128 );

    // Ascending, contiguous region bounds vs. the actual file size:
    //   header(144) | params | [seek table] | payload
    if( paramsOffset != kHeaderSize ) {
        close();
        return false;
    }
    uint64_t cursor = paramsOffset + paramsLen;
    if( cursor < paramsOffset ) { close(); return false; }   // overflow guard

    if( seekTableOffset != 0 ) {
        if( seekTableOffset < cursor ) { close(); return false; }
        cursor = seekTableOffset + seekTableLen;
        if( cursor < seekTableOffset ) { close(); return false; }
    } else if( seekTableLen != 0 ) {
        close();
        return false;
    }

    if( payloadOffset < cursor ) { close(); return false; }
    uint64_t payloadEnd = payloadOffset + payloadLen;
    if( payloadEnd < payloadOffset ) { close(); return false; }  // overflow guard
    if( payloadEnd > fileSize ) { close(); return false; }

    // Load the params blob.
    info.params.resize( (size_t)paramsLen );
    if( paramsLen > 0 ) {
        if( TW_FSEEK64( f_, (int64_t)paramsOffset, SEEK_SET ) != 0 ) {
            close();
            return false;
        }
        if( std::fread( info.params.data(), 1, (size_t)paramsLen, f_ ) != (size_t)paramsLen ) {
            close();
            return false;
        }
    }

    info.payloadLen = payloadLen;
    info_          = std::move( info );
    payloadOffset_ = payloadOffset;
    return true;
}

bool twQafReader::readPayload( void *dest, uint64_t offset, uint64_t len )
{
    if( !f_ )
        return false;
    if( len == 0 )
        return true;
    // Bounds: [offset, offset+len) must lie inside the payload region.
    uint64_t end = offset + len;
    if( end < offset )                 // overflow
        return false;
    if( end > info_.payloadLen )
        return false;

    if( TW_FSEEK64( f_, (int64_t)( payloadOffset_ + offset ), SEEK_SET ) != 0 )
        return false;
    return std::fread( dest, 1, (size_t)len, f_ ) == (size_t)len;
}

bool twQafReader::readRecords( void *dest, uint64_t first, uint64_t count )
{
    if( !f_ )
        return false;
    const uint64_t stride = info_.recordStride;
    if( stride == 0 )                  // variable-stride: not record-addressable
        return false;
    if( count == 0 )
        return true;

    // Guard the stride multiplications against overflow.
    if( first > info_.payloadLen / stride )
        return false;
    if( count > info_.payloadLen / stride )
        return false;

    const uint64_t byteOffset = first * stride;
    const uint64_t byteLen    = count * stride;
    return readPayload( dest, byteOffset, byteLen );
}

bool twQafReader::readAllPayload( std::vector<uint8_t> &out )
{
    if( !f_ )
        return false;
    out.resize( (size_t)info_.payloadLen );
    return readPayload( out.data(), 0, info_.payloadLen );
}
