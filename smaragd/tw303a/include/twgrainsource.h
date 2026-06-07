
#ifndef _TWGRAINSOURCE_H_
#define _TWGRAINSOURCE_H_

#include <vector>

#include "twrandomsource.h"
#include "twgrainparams.h"

/**
 * A time-stretched / pitch-shifted view of another twRandomSource, produced by
 * grain overlap-add (proposal 06).
 *
 * Like twResampledSource, it materialises the whole transformed signal ONCE into
 * a resident planar Float32 buffer in its constructor (normalised overlap-add),
 * after which read() is a lock-free memcpy. This is the "warped source" of
 * proposal 06 §7.2 for CONSTANT stretch/pitch — built once, cacheable, shareable
 * across cuts. Variable (automated) rate will need a streaming node instead.
 *
 * Time-stretch changes grain spacing (output hop vs input hop) so duration
 * changes while pitch is preserved; the pitch offset additionally resamples each
 * grain's content, so pitch and time are independent.
 */
class twGrainSource
    : public twRandomSource
{
public:
    twGrainSource( const twRandomSource &src, const twGrainParams &params );
    virtual ~twGrainSource();

    virtual length_t read( offset_t srcOffset, sample_t *dest,
                           length_t len, idx_t channel ) const;
    virtual length_t length()     const { return nFrames_; }
    virtual idx_t    channels()   const { return channels_; }
    virtual int      sampleRate() const { return rate_; }
    virtual bool     isReproducible() const { return reproducible_; }

private:
    int      rate_;
    idx_t    channels_;
    length_t nFrames_;
    bool     reproducible_;
    std::vector<sample_t> data_;   // planar Float32, size channels_ * nFrames_
};

#endif
