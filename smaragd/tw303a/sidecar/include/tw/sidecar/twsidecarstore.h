
#ifndef _TWSIDECARSTORE_H_
#define _TWSIDECARSTORE_H_

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

#include "tw/core/twcontenthash.h"
#include "tw/sidecar/twqaf.h"

/**
 * The derived-data sidecar store (proposal 27): a size-capped, LRU-evicted
 * directory of QAF files addressed by (content hash × aspect id × params
 * hash). Everything in it is DERIVED — safe to delete wholesale; absence of a
 * file just means the consumer recomputes and re-stores.
 *
 * Layout:  <root>/<hh>/<contenthash32hex>.<aspectId>.<paramshash16hex>.qaf
 * where <hh> is the first two hex chars of the content hash.
 *
 * Lifecycle rules (the invariants):
 *  1. load() validates identity — aspect id, aspect version, content hash,
 *     and params hash are all part of the match. Any mismatch = miss. A file
 *     with a mismatched aspectVersion is deleted on sight (version
 *     orphaning): it can never become valid again.
 *  2. store() is atomic (via twQafWriter) and then enforces the size cap.
 *  3. Eviction is LRU by file mtime; load() touches mtime on a hit. Files
 *     that cannot be deleted (open handle / mapped view — a Windows reality)
 *     are SKIPPED and eviction continues with the next candidate; they are
 *     retried on the next eviction pass. Eviction never fails the caller.
 *  4. An unset root disables the store: load() misses, store() no-ops. The
 *     engine must behave identically (minus speed) with the store disabled —
 *     sidecars are caches, never sources of truth.
 *
 * Threading: all methods are internally serialized with one mutex; callers
 * may use the store from any non-RT thread. The RT audio thread must never
 * call into it (file I/O).
 *
 * The process-wide default instance is configured by the app at startup
 * (root path from the platform cache location; env overrides:
 * SMARAGD_SIDECAR_DIR=<path> relocates it, SMARAGD_SIDECAR_DIR=off disables).
 * Tests construct their own instances on temp dirs.
 */
class twSidecarStore {
public:
    twSidecarStore() = default;

    static twSidecarStore &instance();

    // utf8Path empty = disabled. Creates the directory if needed; if creation
    // fails the store stays disabled (TW_LOGW once).
    void setRoot( const std::string &utf8Path );
    bool enabled() const;

    void     setSizeCapBytes( uint64_t cap ); // default 2 GiB
    uint64_t sizeCapBytes() const;

    std::filesystem::path pathFor( const twContentHash &content,
                                   const std::string &aspectId,
                                   uint64_t paramsHash ) const;

    // Hash a params blob for keying (empty blob hashes to a fixed value —
    // still a valid key component).
    static uint64_t hashParams( const void *params, size_t len );

    // Open-and-validate. Returns an open reader on a full identity match
    // (rule 1), nullptr on any miss. Touches mtime on hit (rule 3).
    std::unique_ptr<twQafReader> load( const twContentHash &content,
                                       const std::string &aspectId,
                                       uint32_t aspectVersion,
                                       uint64_t paramsHash );

    // Params-AGNOSTIC variant lookup (proposal 27 M4): consumers that do not
    // know the producer's params — e.g. the vocoder reading "onsets" written
    // by the import job — take whichever params-variant exists. Validates
    // aspect id + version + content hash like load(); when several variants
    // exist the lexicographically smallest file name wins (deterministic).
    std::unique_ptr<twQafReader> loadAny( const twContentHash &content,
                                          const std::string &aspectId,
                                          uint32_t aspectVersion );

    // Atomic write + size-cap enforcement. info must carry the full identity
    // (contentHash, aspectId, aspectVersion, params); the file name's params
    // hash is computed from info.params. Returns false on write failure
    // (caller keeps its in-memory data; nothing else changes).
    bool store( const twQafInfo &info, const void *payload, uint64_t payloadLen );

    // Enforce the size cap now (rule 3). Normally called by store().
    void evictIfNeeded();

private:
    mutable std::mutex    mutex_;
    std::filesystem::path root_;
    bool                  enabled_ = false;
    uint64_t              sizeCap_ = 2ull * 1024 * 1024 * 1024;
};

#endif
