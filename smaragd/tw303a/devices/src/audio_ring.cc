#include "tw/devices/audio_ring.h"

#include <algorithm>
#include <cstring>

namespace audio {

void AudioRing::reset(std::uint32_t channels, std::size_t capacityFrames)
{
    channels_ = channels ? channels : 1;

    std::size_t cap = 1;
    while (cap < capacityFrames) cap <<= 1;
    capacity_ = cap;
    mask_ = cap - 1;

    buf_.assign(capacity_ * channels_, 0.0f);
    head_.store(0, std::memory_order_relaxed);
    tail_.store(0, std::memory_order_relaxed);
    resetStats();
}

void AudioRing::resetStats()
{
    pushed_.store(0, std::memory_order_relaxed);
    popped_.store(0, std::memory_order_relaxed);
    overrun_.store(0, std::memory_order_relaxed);
    underrun_.store(0, std::memory_order_relaxed);
}

std::size_t AudioRing::availableFrames() const
{
    const std::uint64_t h = head_.load(std::memory_order_acquire);
    const std::uint64_t t = tail_.load(std::memory_order_acquire);
    return (std::size_t)(h - t);
}

std::size_t AudioRing::freeFrames() const
{
    return capacity_ - availableFrames();
}

std::size_t AudioRing::push(const float *interleaved, std::size_t frames)
{
    if (!interleaved || frames == 0 || capacity_ == 0) return 0;

    const std::uint64_t h = head_.load(std::memory_order_relaxed);
    const std::uint64_t t = tail_.load(std::memory_order_acquire);
    const std::size_t   space = capacity_ - (std::size_t)(h - t);
    const std::size_t   n = std::min(frames, space);

    if (n < frames)
        overrun_.fetch_add(frames - n, std::memory_order_relaxed);
    if (n == 0) return 0;

    // Two memcpys at most: the write may wrap the buffer once.
    const std::size_t start = (std::size_t)(h & mask_);
    const std::size_t first = std::min(n, capacity_ - start);
    std::memcpy(&buf_[start * channels_], interleaved,
                first * channels_ * sizeof(float));
    if (n > first)
        std::memcpy(&buf_[0], interleaved + first * channels_,
                    (n - first) * channels_ * sizeof(float));

    head_.store(h + n, std::memory_order_release);
    pushed_.fetch_add(n, std::memory_order_relaxed);
    return n;
}

std::size_t AudioRing::pop(float *interleaved, std::size_t frames)
{
    if (!interleaved || frames == 0 || capacity_ == 0) return 0;

    const std::uint64_t t = tail_.load(std::memory_order_relaxed);
    const std::uint64_t h = head_.load(std::memory_order_acquire);
    const std::size_t   avail = (std::size_t)(h - t);
    const std::size_t   n = std::min(frames, avail);

    // A SHORT read counts as an underrun; an EMPTY ring does not. The
    // difference is the caller's situation: RecordingSession polls an idle
    // device thousands of times a second and gets nothing, which is normal and
    // must not inflate a diagnostic; a consumer that got 300 of the 1024 frames
    // it needed has a real hole to report.
    if (n > 0 && n < frames)
        underrun_.fetch_add(frames - n, std::memory_order_relaxed);
    if (n == 0) return 0;

    const std::size_t start = (std::size_t)(t & mask_);
    const std::size_t first = std::min(n, capacity_ - start);
    std::memcpy(interleaved, &buf_[start * channels_],
                first * channels_ * sizeof(float));
    if (n > first)
        std::memcpy(interleaved + first * channels_, &buf_[0],
                    (n - first) * channels_ * sizeof(float));

    tail_.store(t + n, std::memory_order_release);
    popped_.fetch_add(n, std::memory_order_relaxed);
    return n;
}

void AudioRing::clear()
{
    tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release);
}

}  // namespace audio
