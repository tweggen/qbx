
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

    // --- Page width (proposal 36 §4.2 / §4.3, milestone B3) ----------------
    //
    // THE FIRST PRODUCTION COMPONENT THAT IS EVER WIDER THAN ONE CHANNEL. Its
    // width is the SOURCE's channel count — the same number getNOutputs() has
    // always reported and the same number createOutputLatches() has always
    // built latches for. Until B3 only latch 0 was ever frozen, so a stereo
    // file's channel 1 was computed by nobody; §4.4 rule (1) in
    // twStreamingLatch::copyData is what finally gives those latches meaning.
    //
    // getOutputChannels() and getNOutputs() agree HERE and only here, and that
    // is a coincidence of this class rather than a licence to merge them (§7
    // trap 8): a reader's ports ARE its channels, twRewire's are buses, and
    // twWavInput's were a hardcoded 4.
    virtual idx_t getOutputChannels() const override;

    // Fill EVERY channel of the page in ONE pass, from ONE seek, with ONE
    // cursor advance (§4.3). A per-channel loop over calcOutputTo() would
    // advance pos_ by a whole page per channel and fill channel 1 with the NEXT
    // page's audio — the page-displacement bug this repo has already bled for,
    // and the reason the proposal forbids a generic default loop.
    //
    // TRAP 18: renderFrames() is deliberately NOT overridden. The base
    // renderFrames() routes to calcOutputTo(), which this class DOES override,
    // so a mono scratch page handed to this component still renders channel 0
    // through the narrow path instead of falling into the base
    // renderFrames()/calcOutputTo() mutual recursion.
    virtual length_t renderPageWide( twOutputPage &page, length_t frames,
                                     const sample_t *input,
                                     length_t inputLength ) override;

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
