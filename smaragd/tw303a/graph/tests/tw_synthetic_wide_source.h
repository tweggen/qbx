#ifndef _TW_SYNTHETIC_WIDE_SOURCE_H_
#define _TW_SYNTHETIC_WIDE_SOURCE_H_

// A TEST-ONLY 4-channel component (proposal 36, pulled forward into B1 from
// v2's B2 because it is the ONLY thing that can catch a wrong conversion).
//
// The width-1 byte gate cannot see a mis-converted call site — at width 1 every
// channel index is 0, so `channelPtr(0)` and a mistake spelled `channelPtr(0)`
// are the same instruction. Neither can the grep: a converted call site and a
// WRONGLY converted call site both pass it. What is left is a page that really
// has four channels, with four signals that cannot be mistaken for one another,
// read back through the accessor.
//
// SIGNAL. Channel c carries a square wave of half-period (8 << c) frames around
// a DC offset of (c+1)/8, with amplitude (c+1)/64:
//
//     channel 0:  0.125 +/- 0.015625   ->  {0.109375, 0.140625}   period 16
//     channel 1:  0.250 +/- 0.031250   ->  {0.218750, 0.281250}   period 32
//     channel 2:  0.375 +/- 0.046875   ->  {0.328125, 0.421875}   period 64
//     channel 3:  0.500 +/- 0.062500   ->  {0.437500, 0.562500}   period 128
//
// Every value is dyadic, so float comparison is EXACT — an approximate check
// would be a second thing that could be wrong. And the four value RANGES DO NOT
// OVERLAP, so a SINGLE SAMPLE names its channel; the four periods then name it a
// second, independent way. That is what "unambiguous" has to mean here: not
// "the channels differ" (two channels could differ and still be swapped), but
// "any sample of any channel identifies exactly one channel".
//
// SHAPE. renderWide() seeks once, fills every channel in ONE pass and advances
// its cursor ONCE — the shape §4.3 requires of a wide component, and the reason
// a per-channel loop over renderFrames() must never be built: a cursor-bearing
// component looped per channel would fill channel 1 with the NEXT page's audio.
// The cursor here exists to make that property testable.
//
// AT B1b THIS IS A UNIT PROBE ONLY. It is never added to a graph, never
// scheduled, and no production component declares a width > 1. B2 promotes it to
// a real graph participant, which is why it lives in a header rather than inside
// one test's .cc.

#include "tw/graph/twcomponent.h"
#include "tw/graph/twlatch.h"
#include "tw/pages/tw_output_page.h"

#include <cstdint>
#include <memory>

class twSyntheticWideSource : public twComponent
{
public:
    static constexpr std::uint16_t kChannels = 4;

    explicit twSyntheticWideSource( tw303aEnvironment &e ) : twComponent( e ) {}

    // The signal, as a pure function of (channel, absolute frame). A test can
    // predict any sample without running the component.
    static float value( idx_t c, offset_t frame )
    {
        const float dc  = (float)( c + 1 ) * 0.125f;
        const float amp = (float)( c + 1 ) * 0.015625f;
        const offset_t halfPeriod = (offset_t)( 8 << c );
        const bool low = ( ( frame / halfPeriod ) & 1 ) != 0;
        return low ? dc - amp : dc + amp;
    }

    /// Frames between two sign flips of channel c. A second, independent
    /// identifier: 8, 16, 32, 64.
    static offset_t halfPeriod( idx_t c ) { return (offset_t)( 8 << c ); }

    /// Fill EVERY channel of `page` in one pass, from one seek, with ONE cursor
    /// advance. Returns the frames written per channel.
    length_t renderWide( twOutputPage &page, offset_t startPos, length_t frames )
    {
        if( frames < 0 ) frames = 0;
        if( (size_t)frames > page.channelFrames() ) {
            frames = (length_t)page.channelFrames();
        }

        cursor_ = startPos;                       // seek once
        ++seekCount_;

        const idx_t n = (idx_t)page.channels();
        for( idx_t c = 0; c < n; ++c ) {
            float *dst = page.channelPtr( c );
            for( length_t i = 0; i < frames; ++i ) {
                dst[i] = value( c, cursor_ + (offset_t)i );
            }
        }

        cursor_ += frames;                        // advance once, not per channel
        ++advanceCount_;

        page.validFrames = (uint32_t)frames;
        page.setValidAspects( twAspectAll );
        return frames;
    }

    offset_t cursor() const { return cursor_; }
    int seekCount() const { return seekCount_; }
    int advanceCount() const { return advanceCount_; }

    // --- twComponent contract (the minimum that makes this a component) ---
    void reset() override { cursor_ = 0; }
    void createOutputLatches() override
    {
        pOutputLatches_[0] =
            std::make_shared<twStreamingLatch>( shared_from_this(), 0, 0 );
    }
    idx_t getNInputs() const override { return 0; }
    idx_t getNOutputs() const override { return 1; }
    const char *getInputName( idx_t ) const override { return nullptr; }
    const char *getOutputName( idx_t ) const override { return "out"; }

private:
    offset_t cursor_ = 0;
    int seekCount_ = 0;
    int advanceCount_ = 0;
};

#endif  // _TW_SYNTHETIC_WIDE_SOURCE_H_
