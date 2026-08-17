
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
    // THERE IS NO "capture a live component" CONSTRUCTOR (proposal 36 §7 trap
    // 27, deleted at B9). One existed — `(env, source, captureStart, nFrames,
    // channels, sampleRate)` — and had ZERO callers for its whole life, which
    // is the only reason it was harmless: its body was a per-channel loop that
    // seekTo'd a cursor-bearing component and pulled calcOutputTo once per
    // channel, which is EXACTLY the shape §4.3 forbids. Rendering channel 0,
    // advancing the source's cursor a whole buffer, then asking for channel 1
    // fills channel 1 with the NEXT window's audio — the "coherent page
    // displaced by one page" bug this repo has already bled for. It survived
    // review by being dead, and would have become a live trap the moment
    // anybody wired it, so it goes rather than getting a comment.
    //
    // If a live component ever does need capturing, it must be ONE wide pass:
    // freeze the source's pages and copy every channel out of each page, the
    // way SCut::buildCapture_ does since B7.

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
    // Report data_'s size to tw::pages::PageAccounting and remember what was
    // reported, so the release in the destructor is exact even if the buffer was
    // resized after construction (proposal 36 B7 — a capture is the one
    // allocation in the engine that multiplies by channel width, and it had no
    // instrument at all).
    void accountBuffer_();

    int      sampleRate_;
    idx_t    channels_;
    length_t nFrames_;
    std::vector<sample_t> data_;   // planar Float32, size channels_ * nFrames_
    size_t   accountedBytes_ = 0;
};

#endif
