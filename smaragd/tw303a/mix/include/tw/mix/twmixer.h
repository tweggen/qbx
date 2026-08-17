
#ifndef _TW_MIXER_
#define _TW_MIXER_

#include "tw/graph/twcomponent.h"

#include <atomic>

class tw303aEnvironment;

class twMixer : public twComponent {
    virtual void reset() override;
    virtual void teardown() override;
private:
    idx_t mixerInputs_;
    std::atomic<int> channels_{ 1 };   // see setChannels()
    struct InputProperties {
        double volume_;
        sample_t volumeFactor_;
    };
    InputProperties *inputProperties_;

    // Helpers: do work outside lock (caller must hold mutex)
    int setNInputs_nolock(idx_t n);
    void setBufferSize_nolock(length_t newSize);
    int seekTo_nolock(offset_t offset);

protected:
    sample_t *inBuffer;

public:
    // Phase 3: IOVector-based interface (type-safe, page-backed)
    virtual length_t calcOutputTo( IOVector& dest, idx_t idx ) override;

    // --- Page width (proposal 36 §4.2 / B4) -------------------------------
    //
    // The master sum. Its INPUTS (mixerInputs_) are tracks and have nothing to
    // do with its CHANNELS — the two numbers were conflated by the old "one
    // twMixer per bus" arrangement, in which SStdMixer built N mixers and
    // summed each track's bus n into mixer n. Now there is ONE mixer of width
    // N: input t contributes its page's channel twPageClampChannel(page, c) to
    // output channel c, so a narrower track still reaches every channel (§4.4)
    // and AC B4.1's "master channel k == the sum of the tracks' channel k"
    // holds by construction rather than by wiring.
    void setChannels( idx_t n );
    virtual idx_t getOutputChannels() const override
    {
        return (idx_t) channels_.load( std::memory_order_acquire );
    }

    // The wide render (§4.3): read every wired input's PAGE (§4.4 rule 2), sum
    // channel-wise, one pass, no cursor of our own to advance.
    virtual length_t renderPageWide( twOutputPage &page, length_t frames,
                                     const sample_t *input,
                                     length_t inputLength ) override;

    void createOutputLatches( void ) override;

    void init( void ) override;
    int setNInputs( idx_t );
    int setInputLevel( idx_t, double );
    /// The level input `i` was last set to, in dB (0 == unity). Added by
    /// proposal 21 L1a for the MASTER-SHAPE PRECONDITION (design D3): the
    /// "root(unarmed) + ring" split is legal only while the master really is a
    /// UNITY sum, and a precondition that could not read the levels back would
    /// have to be assumed rather than checked. Returns 0 dB for an index that
    /// has no properties yet, which is what an unwired input contributes.
    double inputLevel( idx_t ) const;
    virtual int seekTo( offset_t offset ) override;
    virtual idx_t getNInputs() const override { return mixerInputs_; }
    virtual idx_t getNOutputs() const override { return 1; }
    virtual const char *getInputName( idx_t ) const override;
    virtual const char *getOutputName( idx_t ) const override;
    
    twMixer( tw303aEnvironment &env, idx_t inputs );
    void setBufferSize( length_t newSize ) override;
};

#endif
