
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
    virtual void reset() override;  // Reset position to start of sample

    // Phase 3: New IOVector-based interface (type-safe, page-backed)
    virtual length_t calcOutputTo( IOVector& dest, idx_t idx ) override;

    // DEPRECATED: Raw-pointer interface (will be removed in v1.0)
    // See: docs/COMPONENT_MIGRATION_GUIDE.md for migration path
    [[deprecated("Use IOVector-based calcOutputTo() or freezePage() instead")]]
    virtual length_t calcOutputTo( sample_t *pDest, length_t length, idx_t idx ) override;

    virtual void createOutputLatches();

    virtual idx_t getNInputs() const;
    virtual idx_t getNOutputs() const;
    virtual const char *getInputName( idx_t ) const;
    virtual const char *getOutputName( idx_t ) const;

    // Internal state snapshot for sequential rendering resumption
    virtual std::any captureInternalState() const override;
    virtual void restoreInternalState(const std::any& state) override;

private:
    // State snapshot type for capture/restore
    struct InternalState {
        offset_t position;
    };

    // Helper: do seek work outside lock (caller must hold mutex)
    int seekTo_nolock(offset_t newOffset);

    // Helper: do output work outside lock (caller must hold mutex)
    length_t calcOutputTo_nolock(sample_t *pDest, length_t length, idx_t idx);

    // Helper: do reset work outside lock (caller must hold mutex)
    void reset_nolock();

    twRandomSource &src_;
    offset_t pos_;
};

#endif
