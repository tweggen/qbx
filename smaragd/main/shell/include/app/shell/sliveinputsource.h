#ifndef _SLIVEINPUTSOURCE_H_
#define _SLIVEINPUTSOURCE_H_

#include <atomic>
#include <cstddef>
#include <vector>

#include "tw/core/twtypes.h"
#include "tw/playback/twliveplan.h"

namespace audio {
class CaptureBridge;
}

/**
 * An `AudioInput` device ring, seen as a `twLiveInputSource` (proposal 21
 * L1b, design D1/D9).
 *
 * `pull()` runs ON THE PUMP THREAD and is ALLOCATION-FREE: the planar scratch
 * and the channel map are sized once, on the main thread, when the plan is
 * built. It must not block, must not take a component mutex and must not touch
 * Qt, which is why the device is reached through `CaptureBridge::pullLive`
 * -- a ring POP, no lock, no allocation -- and nothing else.
 *
 * SINCE L3b THE DEVICE IS THE BRIDGE'S, not this object's and not the
 * monitor's directly (design D7: ONE input pump, three sinks). The bridge
 * thread is the input ring's single consumer and fans the frames out; this is
 * the LIVE sink and the only consumer of `pullLive()`. Recording is a second
 * sink on the SAME bridge, which is what makes "monitoring and recording read
 * one device" true rather than aspirational.
 *
 * `pos` is IGNORED. A device ring is a stream: what it has is what was
 * captured, and the pump stamps the block with the position it is rendering
 * at. Only a synthetic source (the L1a harness) is a function of position.
 *
 * THE CHANNEL MASK is the second half of `audio:<device>:<mask>` -- a hex bit
 * set over the DEVICE's channels. The selected channels are laid onto the
 * track's channels in order, and the last one repeats when the track is wider
 * than the selection: monitoring a mono microphone on a stereo track is the
 * common case and "silence on the right" would be a bug report. Mask 0 means
 * "channel 0", which is what an unqualified `audio:` spelling asks for.
 */
class SLiveAudioInputSource : public twLiveInputSource
{
public:
    SLiveAudioInputSource( audio::CaptureBridge *bridge, unsigned mask,
                           idx_t outChannels, length_t blockFrames );

    std::size_t pull( float *const *out, std::size_t channels,
                      std::size_t frames, offset_t pos ) override;

    /// The peak of everything pulled since the last call, 0..1, then reset.
    /// The pre-FX input meter reads it from the main thread; the pump writes
    /// it. A plain relaxed atomic: a meter never synchronises anything.
    double takePeak();
    /// The peak without clearing it (diagnostics, `describe()`).
    double peekPeak() const
    { return peak_.load( std::memory_order_relaxed ); }
    std::uint64_t framesPulled() const
    { return pulled_.load( std::memory_order_relaxed ); }

private:
    audio::CaptureBridge *bridge_ = nullptr; // borrowed; SLiveMonitor owns it
    std::vector<int>     srcOfOut_;          // out channel c <- device channel
    std::vector<float>   scratch_;           // planar, devCh x blockFrames
    std::vector<float *> planes_;            // into scratch_, one per device ch
    unsigned             devChannels_ = 1;
    std::atomic<double>  peak_{ 0.0 };
    std::atomic<std::uint64_t> pulled_{ 0 };
};

#endif // _SLIVEINPUTSOURCE_H_
