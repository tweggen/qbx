#ifndef _TWRESAMPLER_H_
#define _TWRESAMPLER_H_

#include "twcomponent.h"   // sample_t, length_t, twLatchStreamingOutput

#include <cstdint>
#include <vector>

// Stateful linear sample-rate converter for a single mono channel of sample_t
// (Float32). It pulls input on demand from a streaming-latch output and emits a
// requested number of output frames at outRate. When inRate == outRate it is a
// passthrough: it reads straight through with no interpolation and no copy
// beyond what the underlying read already does.
//
// Quality is intentionally linear for now; the proposal (04) earmarks a
// polyphase / windowed-sinc upgrade behind this same interface. The point of
// this first cut is to remove the pitch/speed error, not to be mastering-grade.
class twResampler
{
public:
    twResampler();

    // (Re)configure the conversion ratio. Resets interpolation state only when
    // the rates actually change, so calling it every callback is cheap.
    void configure( std::uint32_t inRate, std::uint32_t outRate );

    std::uint32_t inRate()  const { return inRate_; }
    std::uint32_t outRate() const { return outRate_; }
    bool isPassthrough()    const { return inRate_ == outRate_; }

    // Hint the largest output block size so the input history buffer can be
    // sized once, up front, instead of growing inside the realtime callback.
    void reserveHint( length_t maxOutFrames );

    // Produce up to outFrames mono samples into `out`, pulling input from `src`.
    // Returns the number of output frames actually produced (short only when the
    // source ran dry). If `inConsumed` is non-null it receives the number of
    // INPUT frames consumed — the caller advances any stream/locator position by
    // this, never by the output count, since the two differ once resampling.
    length_t process( twLatchStreamingOutput *src,
                      sample_t *out, length_t outFrames,
                      length_t *inConsumed = nullptr );

    // Drop carried-over input and interpolation phase (e.g. on play start).
    void reset();

private:
    std::uint32_t inRate_;
    std::uint32_t outRate_;
    // Position of the next output sample, expressed in input samples, measured
    // from hist_[0]. Always in [0, 1) after each process() call's compaction.
    double                phase_;
    std::vector<sample_t> hist_;   // input pulled but not yet fully consumed
};

#endif
