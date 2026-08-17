#include "precise_waiter.h"

#include "tw/devices/midi_out_scheduler.h"

#include <chrono>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace audio {

namespace {

std::chrono::steady_clock::time_point tpFromNs(std::int64_t ns)
{
    return std::chrono::steady_clock::time_point(std::chrono::nanoseconds(ns));
}

}  // namespace

void PreciseWaiter::open()
{
#if defined(_WIN32)
    if (winTimer_ || winWake_) return;
    // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION (Windows 10 1803+). Rejected with
    // ERROR_INVALID_PARAMETER on older systems, so a failure falls back to an
    // ordinary timer rather than leaving us with none.
    const DWORD kHighRes = 0x00000002;
    HANDLE t = CreateWaitableTimerExW(nullptr, nullptr, kHighRes, TIMER_ALL_ACCESS);
    if (!t) t = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    winTimer_ = t;
    winWake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
#endif
}

void PreciseWaiter::close()
{
#if defined(_WIN32)
    if (winTimer_) { CloseHandle((HANDLE) winTimer_); winTimer_ = nullptr; }
    if (winWake_)  { CloseHandle((HANDLE) winWake_);  winWake_  = nullptr; }
#endif
}

void PreciseWaiter::waitUntil(std::int64_t deadlineNs)
{
#if defined(_WIN32)
    if (winTimer_ && winWake_) {
        const std::int64_t relNs = deadlineNs - MidiOutScheduler::hostNowNs();
        if (relNs <= 0) return;
        LARGE_INTEGER due;
        due.QuadPart = -(relNs / 100);          // negative == relative, 100 ns units
        if (due.QuadPart == 0) due.QuadPart = -1;
        SetWaitableTimer((HANDLE) winTimer_, &due, 0, nullptr, nullptr, FALSE);
        HANDLE handles[2] = { (HANDLE) winTimer_, (HANDLE) winWake_ };
        WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        CancelWaitableTimer((HANDLE) winTimer_);
        return;
    }
    // No timer: fall through to the portable path rather than spin.
#endif
    std::unique_lock<std::mutex> lk(m_);
    const std::uint64_t seen = seq_;
    cv_.wait_until(lk, tpFromNs(deadlineNs), [this, seen] { return seq_ != seen; });
}

void PreciseWaiter::wake()
{
#if defined(_WIN32)
    if (winWake_) { SetEvent((HANDLE) winWake_); return; }
#endif
    {
        std::lock_guard<std::mutex> lk(m_);
        ++seq_;
    }
    cv_.notify_one();
}

}  // namespace audio
