
#ifndef _TWSAMPLEREADER_H_
#define _TWSAMPLEREADER_H_

#include "twcomponent.h"

class twRandomSource;

/**
 * A thin, per-consumer cursor over a twRandomSource.
 *
 * This is the "per-stream state" half of the source/reader split (proposal 07):
 * it holds nothing but a read position and a back-reference to the shared,
 * immutable source. Every consumer that wants its own playhead owns one, so two
 * cuts of the same sample no longer fight over a single shared cursor.
 *
 * calcOutputTo() reads at the current position and advances it; seekTo() moves
 * it. The data itself lives in the source and is never copied or owned here.
 */
class twSampleReader
    : public twComponent
{
public:
    twSampleReader( tw303aEnvironment &env, twRandomSource &src );
    virtual ~twSampleReader();

    twRandomSource &getSource() const { return src_; }

    virtual bool isSeekable() const;
    virtual int seekTo( offset_t );
    virtual offset_t tellPos() const;

    virtual length_t calcOutputTo( sample_t *pDest, length_t length, idx_t idx );

    virtual void createOutputLatches();

    virtual idx_t getNInputs() const;
    virtual idx_t getNOutputs() const;
    virtual const char *getInputName( idx_t ) const;
    virtual const char *getOutputName( idx_t ) const;

private:
    twRandomSource &src_;
    offset_t pos_;
};

#endif
