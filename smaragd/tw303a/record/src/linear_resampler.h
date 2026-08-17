#ifndef _TW_RECORD_LINEAR_RESAMPLER_H_
#define _TW_RECORD_LINEAR_RESAMPLER_H_

#include <cstddef>
#include <cstdint>
#include <vector>

namespace audio {

// Stateful streaming linear resampler for interleaved float frames.
//
// Lifted verbatim out of recording_session.cc when proposal 21 L3a moved the
// capture loop into CaptureBridge; it is the same code and the same policy.
// The input backend normally delivers frames already at the project rate
// (WASAPI shared-mode AUTOCONVERTPCM — see WASAPIInput::openDevice), so this is
// a passthrough in the common case, and passthrough() is checked BEFORE it is
// called, so the sample-exact path never runs a single multiply. It only does
// real work on the fallback path, where a driver refused auto-conversion and we
// capture at the device's native mix rate and convert here before the pages and
// the WAV (record CONTRACT inv. 3: files are written at the PROJECT rate).
// Linear quality matches the engine's twResampler bar; that converter pulls
// from a streaming-latch component and does not fit this push-buffer path.
class LinearResampler {
public:
    LinearResampler(std::uint32_t inRate, std::uint32_t outRate, std::uint32_t channels)
        : ratio_(outRate ? static_cast<double>(inRate) / static_cast<double>(outRate) : 1.0),
          channels_(channels ? channels : 1),
          passthrough_(inRate == outRate),
          havePrev_(false),
          frac_(0.0),
          prev_(channels ? channels : 1, 0.0f) {}

    bool passthrough() const { return passthrough_; }

    // Resample `inFrames` interleaved input frames from `in` into `out` (cleared
    // and filled with interleaved output frames). Returns the output frame count.
    std::size_t process(const float *in, std::size_t inFrames,
                        std::vector<float> &out) {
        out.clear();
        if (inFrames == 0) return 0;
        // Upper bound on outputs this chunk; reserve to avoid reallocation churn.
        out.reserve(static_cast<std::size_t>(inFrames / ratio_ + 2.0) * channels_);
        for (std::size_t f = 0; f < inFrames; ++f) {
            const float *cur = in + f * channels_;
            if (!havePrev_) {
                for (std::uint32_t c = 0; c < channels_; ++c) prev_[c] = cur[c];
                havePrev_ = true;
                continue;  // need a previous frame before interpolating an interval
            }
            // Emit every output sample that falls in the interval [prev_, cur).
            while (frac_ < 1.0) {
                for (std::uint32_t c = 0; c < channels_; ++c) {
                    out.push_back(static_cast<float>(
                        prev_[c] * (1.0 - frac_) + cur[c] * frac_));
                }
                frac_ += ratio_;
            }
            frac_ -= 1.0;
            for (std::uint32_t c = 0; c < channels_; ++c) prev_[c] = cur[c];
        }
        return out.size() / channels_;
    }

private:
    double ratio_;            // input frames advanced per output frame
    std::uint32_t channels_;
    bool passthrough_;
    bool havePrev_;
    double frac_;             // position of next output within [prev_, cur), in [0,1)
    std::vector<float> prev_; // last input frame consumed
};

}  // namespace audio

#endif
