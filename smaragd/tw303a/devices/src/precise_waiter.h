#ifndef _TW_PRECISE_WAITER_H_
#define _TW_PRECISE_WAITER_H_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace audio {

// Sleep until a deadline in the MidiOutScheduler::hostNowNs() domain, to ~1 ms,
// wakeable early.
//
// This is MidiOutScheduler's wait, extracted so a second paced thread can use
// it (proposal 21 L0: FileAudioInput's replay thread). The measurement that
// motivated it is recorded in devices inv. 13 and in midi_out_scheduler.cc: on
// this repo's Windows 11 box a std::condition_variable::wait_until rounds up to
// the 15.6 ms system tick EVEN WITH timeBeginPeriod(1) held, so a block of
// 1024 frames (21.3 ms at 48 kHz) would be paced with three quarters of a block
// of jitter. A high-resolution waitable timer measures well under 1 ms.
//
// The scheduler itself still owns its copy: it is on the MIDI timing gate, and
// re-pointing it at this class would put a green, measured path at risk for a
// tidiness that buys nothing. This header is the form a future consolidation
// takes, not that consolidation.
//
// One waiter belongs to ONE waiting thread. wake() may be called from any
// thread; open()/close() are control-plane calls made while the waiter's thread
// is not running.
class PreciseWaiter {
public:
    PreciseWaiter() = default;
    ~PreciseWaiter() { close(); }

    PreciseWaiter(const PreciseWaiter &)            = delete;
    PreciseWaiter &operator=(const PreciseWaiter &) = delete;

    void open();
    void close();

    // Block until `deadlineNs` (hostNowNs domain) or until wake() is called.
    // Returns immediately when the deadline has already passed.
    void waitUntil(std::int64_t deadlineNs);

    // Wake the waiting thread now. Never lost: the Windows path uses an
    // auto-reset event that stays signalled until the next wait consumes it,
    // and the portable path bumps a sequence the predicate compares.
    void wake();

private:
    // void* so no windows.h leaks into anything that includes this.
    void *winTimer_ = nullptr;
    void *winWake_  = nullptr;

    std::mutex              m_;
    std::condition_variable cv_;
    std::uint64_t           seq_ = 0;    // guarded by m_
};

}  // namespace audio

#endif
