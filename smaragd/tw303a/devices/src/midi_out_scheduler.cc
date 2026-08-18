#include "tw/devices/midi_out_scheduler.h"

#include "tw/core/twsyslog.h"

#include <algorithm>
#include <chrono>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <mmsystem.h>
#endif

namespace audio {

namespace {

// How long the sender sleeps when it has nothing pending. It still wakes:
// a producer's notify can in principle be missed only between the predicate
// check and the wait, and a bounded poll turns that theoretical hole into a
// 5 ms one instead of a hang. With something pending the wait ends at the due
// time, so this never costs accuracy.
constexpr std::int64_t kIdlePollNs = 5'000'000;   // 5 ms

// Bound on flush()/panic()'s wait for the sender to become idle. Long enough
// that the sender always gets there, short enough that a wedged backend cannot
// hold the UI thread: it gives up, logs, and lets the caller continue.
constexpr int kDrainWaitMs = 250;

std::chrono::steady_clock::time_point tpFromNs(std::int64_t ns)
{
    return std::chrono::steady_clock::time_point(std::chrono::nanoseconds(ns));
}

}  // namespace

std::int64_t MidiOutScheduler::hostNowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

MidiOutScheduler::MidiOutScheduler(std::unique_ptr<MidiOutput> out)
    : out_(std::move(out))
{
}

MidiOutScheduler::~MidiOutScheduler()
{
    stop();
}

bool MidiOutScheduler::start(const std::string &portId)
{
    if (running_.load(std::memory_order_acquire)) return true;
    if (!out_) return false;

    if (!out_->isOpen() && out_->open(portId) != 0) {
        syslog(LOG_WARNING, "midi: %s backend could not open port '%s'; MIDI out is inactive",
               out_->backendName(), portId.c_str());
        return false;
    }

    openWaitPrimitives();

    // A restart must not inherit the previous run's queue.
    head_.store(0, std::memory_order_relaxed);
    tail_.store(0, std::memory_order_relaxed);
    stopping_.store(false, std::memory_order_relaxed);
    discard_.store(false, std::memory_order_relaxed);
    idle_.store(true, std::memory_order_relaxed);

    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { threadMain(); });
    return true;
}

void MidiOutScheduler::stop()
{
    if (!thread_.joinable()) {
        running_.store(false, std::memory_order_release);
        return;
    }
    stopping_.store(true, std::memory_order_release);
    wake();
    thread_.join();                       // Qt-free join: nothing here is a QObject
    running_.store(false, std::memory_order_release);
    // After the join, so the sender can never wait on a closed handle.
    closeWaitPrimitives();
}

void MidiOutScheduler::resetStats()
{
    sent_.store(0, std::memory_order_relaxed);
    dropped_.store(0, std::memory_order_relaxed);
    late_.store(0, std::memory_order_relaxed);
    maxLateness_.store(0, std::memory_order_relaxed);
}

void MidiOutScheduler::openWaitPrimitives()
{
#if defined(_WIN32)
    // A HIGH-RESOLUTION waitable timer (Windows 10 1803+). This is not
    // belt-and-braces over timeBeginPeriod — it is the thing that works.
    // MEASURED on this repo's Windows 11 box: with timeBeginPeriod(1) held and
    // a std::condition_variable::wait_until, the maximum |sent − due| over 16
    // messages was 15.36 ms, i.e. exactly the 15.6 ms system tick; the wait was
    // rounding up regardless of the requested resolution (Windows 11 may ignore
    // a timer-resolution request from a process it considers background). With
    // the high-resolution timer the same run measures well under 1 ms.
    //
    // The flag is rejected with ERROR_INVALID_PARAMETER on older systems, so a
    // failure falls back to an ordinary timer rather than leaving us with none.
    const DWORD kHighRes = 0x00000002;   // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
    HANDLE t = CreateWaitableTimerExW(nullptr, nullptr, kHighRes, TIMER_ALL_ACCESS);
    if (!t) t = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    winTimer_ = t;
    // Auto-reset, initially unsignalled. A wake() that lands while the sender is
    // working leaves it signalled, so the next wait returns at once — the same
    // no-lost-wakeup property the wakeSeq_ snapshot gives the POSIX path.
    winWake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
#endif
}

void MidiOutScheduler::closeWaitPrimitives()
{
#if defined(_WIN32)
    if (winTimer_) { CloseHandle((HANDLE) winTimer_); winTimer_ = nullptr; }
    if (winWake_)  { CloseHandle((HANDLE) winWake_);  winWake_  = nullptr; }
#endif
}

void MidiOutScheduler::waitUntil(std::int64_t deadlineNs, std::uint64_t seen)
{
#if defined(_WIN32)
    if (winTimer_ && winWake_) {
        const std::int64_t relNs = deadlineNs - hostNowNs();
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
    std::unique_lock<std::mutex> lk(wakeMutex_);
    wakeCv_.wait_until(lk, tpFromNs(deadlineNs), [this, seen] {
        return wakeSeq_ != seen || stopping_.load(std::memory_order_acquire) ||
               discard_.load(std::memory_order_acquire);
    });
}

void MidiOutScheduler::wake()
{
#if defined(_WIN32)
    if (winWake_) { SetEvent((HANDLE) winWake_); return; }
#endif
    {
        std::lock_guard<std::mutex> lk(wakeMutex_);
        ++wakeSeq_;
    }
    wakeCv_.notify_one();
}

bool MidiOutScheduler::enqueue(std::int64_t dueHostTimeNs,
                               const std::uint8_t *bytes, std::size_t size)
{
    if (!bytes || size == 0 || size > kMaxMessageBytes ||
        !running_.load(std::memory_order_acquire)) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    const std::size_t head = head_.load(std::memory_order_relaxed);
    const std::size_t next = (head + 1) % kRingSlots;
    if (next == tail_.load(std::memory_order_acquire)) {
        // Full. One slot is deliberately left unused so head == tail means
        // empty and nothing has to count.
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    Slot &s = ring_[head];
    s.dueHostTimeNs = dueHostTimeNs;
    s.size          = (std::uint8_t) size;
    std::copy(bytes, bytes + size, s.bytes);

    head_.store(next, std::memory_order_release);
    wake();
    return true;
}

bool MidiOutScheduler::sendImmediate(const std::uint8_t *bytes, std::size_t size)
{
    if (!bytes || size == 0 || size > kMaxMessageBytes ||
        !running_.load(std::memory_order_acquire)) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    const std::size_t head = immHead_.load(std::memory_order_relaxed);
    const std::size_t next = (head + 1) % kImmediateSlots;
    if (next == immTail_.load(std::memory_order_acquire)) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    Slot &s = immRing_[head];
    s.dueHostTimeNs = 0;                 // 0 == "at the next opportunity"
    s.size          = (std::uint8_t) size;
    std::copy(bytes, bytes + size, s.bytes);

    immHead_.store(next, std::memory_order_release);
    wake();                              // NOW, not at the next deadline
    return true;
}

void MidiOutScheduler::drainImmediate()
{
    std::size_t tail = immTail_.load(std::memory_order_relaxed);
    const std::size_t head = immHead_.load(std::memory_order_acquire);
    if (tail == head) return;
    const std::int64_t now = hostNowNs();
    while (tail != head) {
        // Sent with due time 0, i.e. never handed to a driver queue even when
        // the backend supports timestamps: a thru message has no future time
        // to be scheduled at, only "as soon as possible".
        sendNow(immRing_[tail], now);
        immSent_.fetch_add(1, std::memory_order_relaxed);
        tail = (tail + 1) % kImmediateSlots;
    }
    immTail_.store(tail, std::memory_order_release);
}

void MidiOutScheduler::drainRing(std::vector<Slot> &pending)
{
    std::size_t tail = tail_.load(std::memory_order_relaxed);
    const std::size_t head = head_.load(std::memory_order_acquire);
    while (tail != head) {
        pending.push_back(ring_[tail]);
        tail = (tail + 1) % kRingSlots;
    }
    tail_.store(tail, std::memory_order_release);
}

void MidiOutScheduler::sendNow(const Slot &s, std::int64_t now)
{
    const bool stamped = out_->supportsTimestamps();
    out_->send(s.bytes, s.size, stamped ? s.dueHostTimeNs : 0);
    sent_.fetch_add(1, std::memory_order_relaxed);

    if (stamped || s.dueHostTimeNs == 0) return;   // the driver owns the timing

    const std::int64_t lateness = now - s.dueHostTimeNs;
    if (lateness > kLateThresholdNs) late_.fetch_add(1, std::memory_order_relaxed);
    if (lateness > maxLateness_.load(std::memory_order_relaxed))
        maxLateness_.store(lateness, std::memory_order_relaxed);
}

void MidiOutScheduler::threadMain()
{
#if defined(_WIN32)
    // Held only while the sender runs, so an idle app leaves the system timer
    // alone. It is NOT what makes the pacing accurate — see openWaitPrimitives()
    // for the measurement that says so — but it still shortens the 1 ms sleeps
    // flush()/panic() do, and costs nothing.
    timeBeginPeriod(1);
#endif

    std::vector<Slot> pending;
    bool warnedOverflow = false;

    while (!stopping_.load(std::memory_order_acquire)) {
        // Snapshot the wake counter BEFORE looking at the ring. An enqueue that
        // lands between the drain below and the wait must make the wait return
        // immediately; taking the snapshot after the drain would swallow
        // exactly that message's wakeup and delay it by a whole poll period.
        std::uint64_t seen;
        {
            std::lock_guard<std::mutex> lk(wakeMutex_);
            seen = wakeSeq_;
        }

        // Not idle from here until this pass has finished sending. Without this
        // store, flush()/panic() could observe the PREVIOUS pass's idle_ == true
        // together with an already-drained ring and return before the messages
        // they are waiting on had actually been sent — and panic() returning
        // early, followed by a stop() that discards, is a stuck note.
        idle_.store(false, std::memory_order_release);

        // MIDI-THRU FIRST, and deliberately OUTSIDE the discard below: a
        // flush() drops the queued FUTURE (a playhead that no longer exists),
        // whereas a thru byte describes a key the performer is pressing right
        // now and has no future to belong to.
        drainImmediate();

        if (discard_.load(std::memory_order_acquire)) {
            // flush(): the queued future belongs to a playhead that no longer
            // exists. Drain the ring INTO the discard, so nothing enqueued
            // before the flush survives it, and only then clear the flag —
            // the producer waits on exactly that ordering.
            drainRing(pending);
            pending.clear();
            discard_.store(false, std::memory_order_release);
        }

        const std::size_t before = pending.size();
        drainRing(pending);
        if (pending.size() != before) {
            // Stable: two messages due at the same instant must reach the wire
            // in the order the pump produced them (a CC before the note-on it
            // sets up).
            std::stable_sort(pending.begin(), pending.end(),
                             [](const Slot &a, const Slot &b) {
                                 return a.dueHostTimeNs < b.dueHostTimeNs;
                             });
        }

        // The ring alone does not bound memory: the sender drains it eagerly,
        // so a producer that enqueues faster than the due times arrive would
        // grow `pending` without limit and the ring would never look full. Cap
        // it at the ring's own size and drop the FURTHEST-FUTURE messages —
        // what is about to be heard matters more than what a runaway pump
        // queued for later, and the drop is counted like any other.
        if (pending.size() > kRingSlots) {
            const std::size_t excess = pending.size() - kRingSlots;
            pending.erase(pending.end() - (std::ptrdiff_t) excess, pending.end());
            dropped_.fetch_add(excess, std::memory_order_relaxed);
            if (!warnedOverflow) {
                warnedOverflow = true;
                syslog(LOG_WARNING,
                       "midi: out queue overflowed %llu messages; the furthest-future"
                       " ones were dropped",
                       (unsigned long long) kRingSlots);
            }
        }

        const std::int64_t now = hostNowNs();

        std::size_t k = 0;
        if (out_->supportsTimestamps()) {
            k = pending.size();          // hand the whole window to the driver
        } else {
            while (k < pending.size() && pending[k].dueHostTimeNs <= now) ++k;
        }
        for (std::size_t i = 0; i < k; ++i) sendNow(pending[i], now);
        if (k) pending.erase(pending.begin(), pending.begin() + (std::ptrdiff_t) k);

        const bool nothingPending =
            pending.empty() && head_.load(std::memory_order_acquire) ==
                                   tail_.load(std::memory_order_relaxed)
            && immHead_.load(std::memory_order_acquire) ==
                   immTail_.load(std::memory_order_relaxed);
        idle_.store(nothingPending, std::memory_order_release);

        const std::int64_t deadline =
            pending.empty() ? hostNowNs() + kIdlePollNs : pending.front().dueHostTimeNs;

        waitUntil(deadline, seen);
    }

    // Whatever is left is dropped on purpose (see stop()).
    idle_.store(true, std::memory_order_release);

#if defined(_WIN32)
    timeEndPeriod(1);
#endif
}

void MidiOutScheduler::flush()
{
    if (!running_.load(std::memory_order_acquire) || !thread_.joinable()) {
        // No sender: the producer owns both ends of the ring, so it may reset
        // it itself.
        tail_.store(head_.load(std::memory_order_relaxed), std::memory_order_release);
        return;
    }

    discard_.store(true, std::memory_order_release);
    wake();

    for (int i = 0; i < kDrainWaitMs; ++i) {
        if (!discard_.load(std::memory_order_acquire) &&
            idle_.load(std::memory_order_acquire))
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    syslog(LOG_WARNING, "midi: flush() timed out after %d ms waiting for the sender to drain",
           kDrainWaitMs);
}

void MidiOutScheduler::panic(std::uint16_t channelMask)
{
    if (!out_) return;

    // The queued future first: a note-on that escapes after an all-notes-off is
    // a stuck note that survives the panic.
    flush();

    const bool inline_ = !running_.load(std::memory_order_acquire) || !thread_.joinable();

    for (int ch = 0; ch < 16; ++ch) {
        if (!(channelMask & (std::uint16_t)(1u << ch))) continue;
        const std::uint8_t status = (std::uint8_t)(0xB0 | (std::uint8_t) ch);
        const std::uint8_t sustainOff[3]   = { status, 64,  0 };   // CC64  = 0
        const std::uint8_t allNotesOff[3]  = { status, 123, 0 };   // CC123 = 0
        if (inline_) {
            // No sender thread: the caller IS the only sender, so writing
            // directly keeps the "one sender at a time" rule intact.
            out_->send(sustainOff, 3, 0);
            out_->send(allNotesOff, 3, 0);
            sent_.fetch_add(2, std::memory_order_relaxed);
        } else {
            enqueue(0, sustainOff, 3);
            enqueue(0, allNotesOff, 3);
        }
    }
    if (inline_) return;

    for (int i = 0; i < kDrainWaitMs; ++i) {
        if (idle_.load(std::memory_order_acquire) &&
            head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire))
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    syslog(LOG_WARNING, "midi: panic() timed out after %d ms waiting for the sender",
           kDrainWaitMs);
}

}  // namespace audio
