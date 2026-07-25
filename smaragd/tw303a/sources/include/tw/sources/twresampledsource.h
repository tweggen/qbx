
#ifndef _TWRESAMPLEDSOURCE_H_
#define _TWRESAMPLEDSOURCE_H_

#include <vector>

#include "tw/sources/twrandomsource.h"

/**
 * A resident, resampled view of another twRandomSource at a different rate.
 *
 * The whole material is resampled ONCE (linear interpolation) into a planar
 * Float32 buffer in the constructor; after that read() is a lock-free memcpy.
 * Being reproducible and self-contained, a single instance can be shared by the
 * preview path and every reader of the same source/rate, so the material is
 * resampled once rather than on every block (proposal 07 §6).
 *
 * Quality is intentionally linear, matching twResampler — the point is to remove
 * the pitch/speed error of playing an off-rate sample, not to be mastering-grade.
 */
class twResampledSource
    : public twRandomSource,
      public std::enable_shared_from_this<twResampledSource>
{
public:
    twResampledSource( const twRandomSource &src, int targetRate );
    virtual ~twResampledSource();

    virtual length_t read( offset_t srcOffset, sample_t *dest,
                           length_t len, idx_t channel ) const;
    virtual length_t length()     const { return nFrames_; }
    virtual idx_t    channels()   const { return channels_; }
    virtual int      sampleRate() const { return targetRate_; }
    virtual bool     isReproducible() const { return true; }

    // Forward the underlying material's digest (captured at construction —
    // the resample is a derived view, not new content; proposal 27).
    virtual twContentHash contentHash() const { return contentHash_; }

    // Resident-planar view + co-ownership handle for the M5 streaming grain
    // (this class is always shared_ptr-owned via the viewAtRate cache).
    virtual const sample_t *channelData( idx_t channel ) const override {
        if( channel < 0 || channel >= channels_ || nFrames_ <= 0 ) return nullptr;
        return data_.data() + (size_t) channel * nFrames_;
    }
    virtual std::shared_ptr<const twRandomSource> sharedRef() const override {
        try {
            return shared_from_this();
        } catch( const std::bad_weak_ptr & ) {
            return nullptr;
        }
    }

private:
    int      targetRate_;
    twContentHash contentHash_;
    idx_t    channels_;
    length_t nFrames_;
    std::vector<sample_t> data_;   // planar Float32, size channels_ * nFrames_
};

#endif
