#ifndef _TW_AUDIO_RING_H_
#define _TW_AUDIO_RING_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace audio {

// The single-producer / single-consumer frame ring every capture device writes
// into (proposal 21 L0, design D5).
//
// Why it exists. Before this, AudioInput::read() WAS the device poll: WASAPI's
// read() called GetBuffer, copied at most the caller's frameCount, and then
// released the WHOLE packet — so every frame past the caller's buffer was
// dropped on the floor and the recorded timeline silently compressed (design
// §1 F7). A capture thread that pushes the whole packet into this ring and a
// read() that pops from it separates "how much the device gave us" from "how
// much the caller asked for", which is the only shape in which the tail cannot
// be lost.
//
// Threading
//   - ONE producer thread (the device's capture thread) calls push().
//   - ONE consumer thread calls pop(). RecordingSession's worker is that
//     consumer today; proposal 21 L1a's pump becomes a second, alternative
//     consumer — never a concurrent one.
//   - head_ is written only by the producer, tail_ only by the consumer; the
//     acquire/release pairing is the whole synchronisation. No lock, no
//     allocation, nothing that can block: the producer runs at device
//     priority and the consumer may be the RT-adjacent pump.
//   - reset()/clear() are CONTROL-PLANE calls, legal only while neither side
//     is running (openDevice / startCapture / stopCapture), exactly like the
//     device handles they sit next to.
//
// Overrun policy: a push that does not fit writes what fits and COUNTS the
// rest in overrunFrames(). It never overwrites unread frames — in SPSC the
// consumer may be mid-copy out of exactly those, so "drop the oldest" is a
// data race, not a policy. A dropped frame is a number a test can assert on;
// a corrupted one is not.
class AudioRing {
public:
    AudioRing() = default;

    // Size the ring. `capacityFrames` is rounded up to a power of two so the
    // index wrap is a mask (the producer runs on a device thread; a modulo is
    // not the cost, but the branchlessness is free). One slot is NOT reserved:
    // head_/tail_ are free-running 64-bit counters, so full and empty are
    // distinguished by their difference, not by an index collision.
    void reset(std::uint32_t channels, std::size_t capacityFrames);

    std::uint32_t channels() const { return channels_; }
    std::size_t capacityFrames() const { return capacity_; }

    // Frames readable right now (consumer's view; safe from either side).
    std::size_t availableFrames() const;
    // Frames writable right now (producer's view).
    std::size_t freeFrames() const;

    // Producer. Returns the number of frames actually accepted; the shortfall
    // is counted in overrunFrames().
    std::size_t push(const float *interleaved, std::size_t frames);

    // Consumer. Returns the number of frames actually delivered (< `frames`
    // when the ring holds less); the shortfall is counted in underrunFrames()
    // ONLY when the ring was non-empty in this call's own sense — see the .cc:
    // an idle device with nothing to give is not an underrun, a device that
    // gave less than a running consumer needed is.
    std::size_t pop(float *interleaved, std::size_t frames);

    // Drop everything readable (control plane; not for the producer).
    void clear();

    std::uint64_t framesPushed() const   { return pushed_.load(std::memory_order_relaxed); }
    std::uint64_t framesPopped() const   { return popped_.load(std::memory_order_relaxed); }
    std::uint64_t overrunFrames() const  { return overrun_.load(std::memory_order_relaxed); }
    std::uint64_t underrunFrames() const { return underrun_.load(std::memory_order_relaxed); }
    void resetStats();

private:
    std::vector<float>       buf_;
    std::size_t              capacity_ = 0;      // frames, power of two
    std::size_t              mask_     = 0;
    std::uint32_t            channels_ = 1;

    std::atomic<std::uint64_t> head_{ 0 };       // frames written (producer)
    std::atomic<std::uint64_t> tail_{ 0 };       // frames read (consumer)

    std::atomic<std::uint64_t> pushed_{ 0 };
    std::atomic<std::uint64_t> popped_{ 0 };
    std::atomic<std::uint64_t> overrun_{ 0 };
    std::atomic<std::uint64_t> underrun_{ 0 };
};

}  // namespace audio

#endif
