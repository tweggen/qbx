
#ifndef _TWCONTENTHASH_H_
#define _TWCONTENTHASH_H_

#include <cstddef>
#include <cstdint>
#include <string>

/**
 * 128-bit content digest for derived-data keying (proposal 27).
 *
 * The digest identifies *decoded source PCM* (all channels, planar Float32, at
 * the source's native rate), independent of filename, container, or mtime, so
 * identical material shares one sidecar across projects. It is a CACHE KEY,
 * not a security primitive: the hash is MurmurHash3 x64-128 (public domain),
 * implemented in-tree so the engine gains no new dependency. Deterministic on
 * all supported targets (little-endian x86-64 / ARM64 assumed; static_assert
 * in the .cc). Changing the algorithm or seed simply orphans existing sidecar
 * files — they are derived data and regenerate.
 *
 * A default-constructed (all-zero) value means "no hash available" and must
 * never be used as a key; twHashBuffer cannot legitimately return it in
 * practice, and consumers gate on isNull().
 */
struct twContentHash {
    uint64_t lo = 0;
    uint64_t hi = 0;

    bool isNull() const { return lo == 0 && hi == 0; }
    bool operator==( const twContentHash &o ) const { return lo == o.lo && hi == o.hi; }
    bool operator!=( const twContentHash &o ) const { return !( *this == o ); }

    // 32 lowercase hex chars, hi first — the on-disk key spelling.
    std::string toHex() const;
};

// One-shot digest over a resident buffer (the decode assembles the full PCM
// buffer anyway, so no streaming variant is needed).
twContentHash twHashBuffer( const void *data, size_t len,
                            uint64_t seed = 0x00736d6172616764ULL );

#endif
