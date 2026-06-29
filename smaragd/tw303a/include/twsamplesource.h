
#ifndef _TWSAMPLESOURCE_H_
#define _TWSAMPLESOURCE_H_

#include <memory>
#include <vector>
#include <mutex>
#include <map>
#include <qstring.h>

#include "twrandomsource.h"

class tw303aEnvironment;
class twResampledSource;

/**
 * A file-backed, fully-resident random-access sample source.
 *
 * At construction it decodes the entire WAV into RAM as planar Float32
 * (channel-major: channel c, frame f at data_[c*nFrames_ + f]). After that the
 * file handle is closed and read() is a lock-free memcpy out of resident memory
 * — no per-call file I/O and no mutex in the realtime path, which dissolves the
 * UI/audio race that the old twWavInput guarded with a lock (proposal 07 §2/§3).
 *
 * NB: only 16-bit PCM is decoded (matching what twWavInput ever actually
 * supported); other bit depths fail to load.
 */
class twSampleSource
    : public twRandomSource
{
public:
    twSampleSource( tw303aEnvironment &env, const QString &fileName );
    virtual ~twSampleSource();

    bool wasLoaded() const { return loaded_; }
    QString fileName() const { return fileName_; }

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
