
#ifndef _TWCAPTURINGSOURCE_H_
#define _TWCAPTURINGSOURCE_H_

#include <vector>

#include "tw/sources/twrandomsource.h"

class tw303aEnvironment;
class twComponent;

/**
 * A resident, random-access capture of a *linear* twComponent's output
 * (proposal 07 step 5).
 *
 * twResampledSource / twGrainSource materialise another twRandomSource; this one
 * materialises a streaming, stateful twComponent — e.g. a track or mixer bus
 * (twTrackMix) — by pulling it once, in the constructor, into a planar Float32
 * buffer. After that read() is a lock-free memcpy.
 *
 * Why it exists: a re-used "live asset" is a cut over a group, and a group's
 * output node is a single-cursor streaming graph. Pulling it once per placement
 * would both re-render the whole sub-graph every time AND fight over its one
 * read cursor (the very hazard proposal 07 removes for samples). Capturing the
 * windowed output into immutable data renders it ONCE and lets every placement
 * mint its own independent reader (acquireReader) over the snapshot — cheap and
 * correct. Identical captures can then be shared via a content-addressed cache
 * (proposal 06 §7).
 *
 * The capture is a SNAPSHOT at construction time: pulling the source advances
 * its cursor, so this must run OFF the audio thread while the source is not
 * being played (the same constraint as the grain materialisation). When the
 * underlying arrangement changes, the owner rebuilds the source (invalidation).
 */
class twCapturingSource
    : public twRandomSource
{
public:
    // Capture `nFrames` frames of `source`, starting at `captureStart`, for
    // `channels` channels, tagged with `sampleRate`. `source` should be seekable
    // so each channel can be captured from the same start (twTrackMix advances
    // one shared cursor per call, regardless of channel).
    twCapturingSource( tw303aEnvironment &env, twComponent &source,
                       offset_t captureStart, length_t nFrames,
                       idx_t channels, int sampleRate );

    // Adopt an already-rendered planar buffer (size channels*nFrames). Used by
    // the recursive container capture (proposal 10 Phase 1), which composes a
    // buffer by reading children RANDOM-ACCESS rather than streaming a live
    // component. Short buffers are zero-padded to channels*nFrames.
    twCapturingSource( std::vector<sample_t> &&data, length_t nFrames,
                       idx_t channels, int sampleRate );

    virtual ~twCapturingSource();

    virtual length_t read( offset_t srcOffset, sample_t *dest,
                           length_t len, idx_t channel ) const;
    virtual length_t length()     const { return nFrames_; }
    virtual idx_t    channels()   const { return channels_; }
    virtual int      sampleRate() const { return sampleRate_; }
    virtual bool     isReproducible() const { return true; }

private:
    int      sampleRate_;
    idx_t    channels_;
    length_t nFrames_;
    std::vector<sample_t> data_;   // planar Float32, size channels_ * nFrames_
};

#endif
