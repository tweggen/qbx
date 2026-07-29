
#ifndef _TWSAMPLESOURCE_H_
#define _TWSAMPLESOURCE_H_

#include <memory>
#include <vector>
#include <mutex>
#include <map>
#include <qstring.h>

#include "tw/core/twcontenthash.h"
#include "tw/sources/twrandomsource.h"

class tw303aEnvironment;
class twResampledSource;

/**
 * A file-backed, fully-resident random-access sample source.
 *
 * At construction it decodes the entire file into RAM as planar Float32
 * (channel-major: channel c, frame f at data_[c*nFrames_ + f]). After that the
 * file handle is closed and read() is a lock-free memcpy out of resident memory
 * — no per-call file I/O and no mutex in the realtime path, which dissolves the
 * UI/audio race that the old twWavInput guarded with a lock (proposal 07 §2/§3).
 *
 * Decoding: 16-bit PCM WAV takes the hand-rolled fast path (loadWav, byte-exact
 * with every prior build). Everything else — MP3, FLAC, AIFF, Ogg/Opus and
 * non-16-bit WAV — decodes through libsndfile (loadSndfile), which yields the
 * identical planar-Float32 buffer, so all downstream readers are format-agnostic.
 * MP3 read requires a libsndfile built with mpg123; unsupported/undecodable files
 * fail to load exactly as before.
 */
class twSampleSource
    : public twRandomSource,
      public std::enable_shared_from_this<twSampleSource>
{
public:
    twSampleSource( tw303aEnvironment &env, const QString &fileName );
    virtual ~twSampleSource();

    bool wasLoaded() const { return loaded_; }
    QString fileName() const { return fileName_; }

    // Digest of the decoded source-rate PCM (all channels, planar Float32) —
    // the derived-data cache key (proposal 27). Computed once inside the
    // load pass; null iff the load failed.
    twContentHash contentHash() const { return contentHash_; }

    // Direct read-only view of one channel's resident source-rate samples
    // (proposal 27 M1: analysis jobs read in place instead of copying the
    // whole buffer). Immutable after load; lock-free from any thread. Null
    // if not loaded or channel out of range.
    virtual const sample_t *channelData( idx_t channel ) const override {
        if( !loaded_ || channel < 0 || channel >= channels_ ) return nullptr;
        return data_.data() + (size_t) channel * nFrames_;
    }

    // Valid only when this instance is shared_ptr-owned (twWavInput does).
    virtual std::shared_ptr<const twRandomSource> sharedRef() const override {
        try {
            return shared_from_this();
        } catch( const std::bad_weak_ptr & ) {
            return nullptr;   // raw-owned instance: streaming consumers must copy
        }
    }

    // twRandomSource
    virtual length_t read( offset_t srcOffset, sample_t *dest,
                           length_t len, idx_t channel ) const;
    virtual length_t length()     const { return loaded_ ? nFrames_ : -1; }
    virtual idx_t    channels()   const { return channels_; }
    virtual int      sampleRate() const { return rate_; }
    virtual bool     isReproducible() const { return true; }

    /**
     * A playable view of this material at targetRate. Returns `this` when the
     * native rate already matches; otherwise a cached resampled view, built once
     * per rate and reused. The whole-object "play me at the project rate" entry
     * point — preview, readers, and duration all go through it so pitch and length
     * stay consistent.
     *
     * Thread-safe: uses std::call_once for lock-free lazy initialization.
     * Multiple resampled views (one per rate) cached efficiently. Constructor
     * called outside lock to avoid blocking on expensive work.
     */
    twRandomSource *viewAtRate( int targetRate ) const;

private:
    int loadWav();
    // libsndfile-backed decoder for MP3/FLAC/AIFF/Ogg/Opus and non-16-bit WAV.
    // Fills data_ (planar Float32), channels_/rate_/nFrames_/bits_ and
    // contentHash_ exactly as loadWav() does. Returns 0 on success, <0 on error.
    int loadSndfile();

    // Cache entry: uses std::call_once to ensure exactly one construction per rate,
    // even with concurrent viewAtRate() calls from different threads.
    struct ResampledEntry {
        std::once_flag flag;
        std::shared_ptr<twResampledSource> obj;
    };

    tw303aEnvironment &env_;
    QString  fileName_;
    bool     loaded_;
    idx_t    channels_;
    int      rate_;
    int      bits_;
    length_t nFrames_;
    twContentHash contentHash_;
    std::vector<sample_t> data_;   // planar Float32, size channels_ * nFrames_

    // Dictionary of resampled views, keyed by target rate.
    // Mutable so viewAtRate() can stay const for const read paths (getLength, preview).
    // Protected by resampledMutex_ to guard dictionary access only (not constructor).
    // Constructor (std::call_once) is called OUTSIDE the lock, avoiding mutex contention
    // for expensive resampler creation.
    mutable std::mutex resampledMutex_;
    mutable std::map<int, ResampledEntry> resampledCache_;
};

#endif
