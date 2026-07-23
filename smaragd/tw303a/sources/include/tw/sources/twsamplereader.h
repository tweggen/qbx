
#ifndef _TWSAMPLEREADER_H_
#define _TWSAMPLEREADER_H_

#include <memory>
#include <vector>

#include "tw/graph/twcomponent.h"

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

    // Anchor an upstream object src_ refers into (grain source, capture
    // snapshot). src_ is a raw reference, and a scheduler PageNode holds ONLY
    // this reader's shared_ptr — so the reader must transitively own
    // everything src_ can reach, or a reader swap (SCut::rebuildReader churn
    // during a drag) / ~SCut frees the grain/capture while a queued freeze
    // still renders through it. Call before publishing the reader; not
    // thread-safe afterwards.
    void retainUpstream( std::shared_ptr<const void> p )
    {
        if( p ) upstreamRefs_.push_back( std::move( p ) );
    }

    virtual bool isSeekable() const override;
    virtual int seekTo( offset_t ) override;
    virtual offset_t tellPos() const override;
    virtual void reset() override;  // Reset position to start of sample

    // Single read cursor (pos_): freezes must be serialized (proposal 19 Ph1).
    // twLoopReader inherits this.
    bool usesSerialCursor() const override { return true; }

    // Phase 3: IOVector-based interface (type-safe, page-backed)
    virtual length_t calcOutputTo( IOVector& dest, idx_t idx ) override;

    // Teardown protocol
    virtual void teardown() override;

    virtual void createOutputLatches() override;

    virtual idx_t getNInputs() const override;
    virtual idx_t getNOutputs() const override;
    virtual const char *getInputName( idx_t ) const override;
    virtual const char *getOutputName( idx_t ) const override;

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

    // Helper: do reset work outside lock (caller must hold mutex)
    void reset_nolock();

    twRandomSource &src_;
    offset_t pos_;

    // Lifetime anchors for the chain behind src_ (see retainUpstream()).
    std::vector<std::shared_ptr<const void> > upstreamRefs_;
};

#endif
