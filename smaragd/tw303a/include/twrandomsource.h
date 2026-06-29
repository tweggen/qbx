
#ifndef _TWRANDOMSOURCE_H_
#define _TWRANDOMSOURCE_H_

#include "twcomponent.h"   // sample_t, length_t, offset_t, idx_t

class tw303aEnvironment;
class twSampleReader;

/**
 * A positionless, read-only, random-access view over sample data.
 *
 * This is the "immutable data" half of the source/reader split (proposal 07).
 * read() carries NO cursor and mutates no shared state, so any number of
 * consumers may read concurrently from different offsets without interfering —
 * which is exactly what the old shared, stateful twWavInput could not do.
 *
 * A source is also a *factory of readers*: acquireReader() mints an independent
 * cursor (a twSampleReader, itself a twComponent) over the same shared data, for
 * consumers that want a linear streaming face instead of random reads.
 */
class twRandomSource
{
public:
    virtual ~twRandomSource() {}

    /**
     * Stateless random read of a single channel. Copies up to `len` frames
     * starting at `srcOffset` into `dest`; any frames at/after end-of-material
     * are zero-filled. `channel` is clamped into [0, channels()-1] (so mono
     * material is served on every requested channel). Returns the number of
     * frames actually backed by real data (0..len).
     */
    virtual length_t read( offset_t srcOffset, sample_t *dest,
                           length_t len, idx_t channel ) const = 0;

    virtual length_t length()     const = 0;   // frames, or -1 if unbounded/live
    virtual idx_t    channels()   const = 0;
    virtual int      sampleRate() const = 0;   // native rate of the material

    /**
     * True iff the same read() arguments always yield the same bytes (a file,
     * an offline render). Live or randomized material returns false. This is the
     * hint the grain cache (proposal 06 §7) keys cross-cut sharing on.
     */
    virtual bool isReproducible() const = 0;
    bool isBounded() const { return length() >= 0; }

    /**
     * Mint an independent cursor over this data at the specified initial offset.
     * The cursor starts at `initialOffset` into the source material (default: 0).
     *
     * The caller OWNS the returned reader and must delete it (before or together
     * with releasing the source).
     *
     * This enables resource-efficient reader initialization: the offset is set at
     * construction time, avoiding separate seekTo() call. Future implementations
     * can use this to avoid materializing data before the offset.
     *
     * Defined in twsamplereader.cc, where twSampleReader is complete.
     */
    twSampleReader *acquireReader( tw303aEnvironment &env, offset_t initialOffset = 0 );
};

#endif
