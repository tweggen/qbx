
#ifndef _TWQAF_H_
#define _TWQAF_H_

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "tw/core/twcontenthash.h"

/**
 * QAF — the qbx analysis file container (proposal 27 §"The QAF format").
 *
 * One file per (content × aspect × params). Self-describing, versioned,
 * CRC-protected fixed header; contiguous payload of records. The format
 * promises exactly one access pattern: random START (fixed-stride records
 * make the seek a multiplication), then SEQUENTIAL read. Payloads are opaque
 * to this layer — the aspect registry (tw/sidecar/twaspects.h) defines what
 * the bytes mean.
 *
 * Binary layout, little-endian, offsets absolute from file start:
 *
 *   off size field
 *     0   4  magic "QAF1"
 *     4   4  formatVersion            (container layout version, == 1)
 *     8  16  aspectId                 (ASCII, NUL-padded)
 *    24   4  aspectVersion            (payload semantics version, per aspect)
 *    28   4  flags                    (== 0, reserved)
 *    32  16  contentHash              (lo u64, hi u64)
 *    48   4  sourceRate               (Hz, native rate of the hashed PCM)
 *    52   4  channels
 *    56   8  sourceFrames             (frame count of the hashed PCM)
 *    64   8  recordStride             (bytes per record; 0 = variable-stride)
 *    72   8  recordCount
 *    80   8  hopFrames                (source/derived frames per record; 0 = n/a)
 *    88   8  paramsOffset             (== 144)
 *    96   8  paramsLen
 *   104   8  seekTableOffset          (0 = none; only for variable-stride aspects)
 *   112   8  seekTableLen
 *   120   8  payloadOffset
 *   128   8  payloadLen
 *   136   4  reserved                 (== 0)
 *   140   4  headerCrc32              (CRC-32/ISO-HDLC over bytes [0,140))
 *   144      params blob, then optional seek table, then payload
 *
 * File layout invariants: header(144) | params | [seek table] | payload,
 * each region contiguous, offsets ascending. Writers produce files atomically
 * (write to "<path>.tmp", then rename over) so a reader never observes a
 * partial file. Any validation failure (magic, version, CRC, region bounds)
 * makes open() fail — a bad sidecar is treated as absent, never "repaired".
 *
 * Threading: a twQafReader/Writer instance is single-threaded; distinct
 * instances on distinct files are freely concurrent. No Qt, no engine state.
 */

struct twQafInfo {
    std::string          aspectId;        // <= 16 ASCII chars
    uint32_t             aspectVersion = 0;
    twContentHash        contentHash;
    uint32_t             sourceRate    = 0;
    uint32_t             channels      = 0;
    uint64_t             sourceFrames  = 0;
    uint64_t             recordStride  = 0;
    uint64_t             recordCount   = 0;
    uint64_t             hopFrames     = 0;
    std::vector<uint8_t> params;
    uint64_t             payloadLen    = 0; // filled by reader; ignored by writer
};

// CRC-32 (ISO-HDLC, the zlib polynomial), used for the header checksum and
// available to aspect payload authors that want one.
uint32_t twCrc32( const void *data, size_t len, uint32_t seed = 0 );

class twQafWriter {
public:
    // One-shot atomic write: header+params+payload to "<path>.tmp", fsync,
    // rename over path. Creates parent directories. Returns false (and
    // TW_LOGW) on any failure; a failed write leaves no partial file behind.
    // info.payloadLen is ignored; payloadLen parameter is authoritative.
    static bool write( const std::filesystem::path &path,
                       const twQafInfo &info,
                       const void *payload, uint64_t payloadLen );
};

class twQafReader {
public:
    twQafReader() = default;
    ~twQafReader();
    twQafReader( const twQafReader & ) = delete;
    twQafReader &operator=( const twQafReader & ) = delete;

    // Parses and validates the header (magic, formatVersion, CRC, region
    // bounds vs. actual file size) and reads the params blob. On failure the
    // reader is closed and false is returned.
    bool open( const std::filesystem::path &path );
    bool isOpen() const { return f_ != nullptr; }
    void close();

    const twQafInfo &info() const { return info_; }

    // Random start, then sequential: byte range [offset, offset+len) of the
    // payload region. Returns false on short read / out-of-bounds request.
    bool readPayload( void *dest, uint64_t offset, uint64_t len );

    // Record-addressed convenience for fixed-stride aspects
    // (recordStride > 0): records [first, first+count).
    bool readRecords( void *dest, uint64_t first, uint64_t count );

    bool readAllPayload( std::vector<uint8_t> &out );

private:
    twQafInfo info_;
    FILE     *f_             = nullptr;
    uint64_t  payloadOffset_ = 0;
};

#endif
