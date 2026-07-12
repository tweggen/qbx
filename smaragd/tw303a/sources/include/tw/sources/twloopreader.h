
#ifndef _TWLOOPREADER_H_
#define _TWLOOPREADER_H_

#include "tw/sources/twsamplereader.h"

/**
 * A reader that loops a fixed segment of its source.
 *
 * It reads the segment [loopBase, loopBase+loopLen) of the underlying
 * twRandomSource and wraps back to loopBase whenever the read crosses the
 * segment end. So a cut that is longer than its loop segment repeats the
 * segment to fill its duration (proposal: clip-edge loop gesture).
 *
 * The cursor (tellPos/seekTo, inherited from twSampleReader) is cut-relative
 * (0-based); the loop base is added internally. With loopLen <= 0 it behaves
 * exactly like a plain twSampleReader (single linear pass). Used by SCut when
 * its loopLength_ is set — see scut.cpp::rebuildReader().
 */
class twLoopReader : public twSampleReader {
public:
    twLoopReader( tw303aEnvironment &env, twRandomSource &src,
                  offset_t loopBase, length_t loopLen );
    virtual ~twLoopReader();

    virtual void reset() override;  // Inherited from twSampleReader

    // Phase 3: IOVector-based interface (type-safe, page-backed)
    virtual length_t calcOutputTo( IOVector& dest, idx_t idx ) override;

private:
    offset_t loopBase_;
    length_t loopLen_;
};

#endif
