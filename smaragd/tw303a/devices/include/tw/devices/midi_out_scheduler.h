#ifndef _TW_MIDI_OUT_SCHEDULER_H_
#define _TW_MIDI_OUT_SCHEDULER_H_

#include "tw/devices/midi_output.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace audio {

// The thing that actually puts bytes on the wire, at the right time.
//
// Proposal 37 D6: MIDI-out is emitted at PLAY time, never at freeze time — the
// metering lesson (34) verbatim, because pages are frozen ~1.4 s ahead of the
// playhead and by renders with no playhead at all. The app's MidiOutPump
// (main-thread QTimer, 20 ms period, 250 ms lookahead — P7b) slices the event
// feed and hands {dueHostTimeNs, bytes} to this class; this class owns the one
// MidiOutput and the one thread that sends.
//
// Threading
//   - enqueue() is SINGLE-PRODUCER. That producer is the app pump, on the main
//     thread. The ring's head is written by nobody else; a second producer
//     would corrupt it silently, so the invariant is stated rather than
//     defended with a lock the pump does not need. panic() and flush() are
//     control-plane calls from the same producer thread.
//   - The sender is one std::thread with NO Qt in it. A Qt signal from here
//     would make Qt adopt the thread, whose TLS cleanup then deadlocks the
//     join at teardown (THREADING.md rule 1) — which is why stop()/the
//     destructor join it directly and publish nothing but atomics.
//
// Pacing: 1 ms granularity (timeBeginPeriod(1) is held on Windows for as long
// as the thread runs — without it a wait rounds up to the 15.6 ms system tick
// and every note is early-or-late by a musically obvious amount). When the
// backend supportsTimestamps() (CoreMIDI, ALSA sequencer queues) the message is
// handed off IMMEDIATELY with its due time attached and the driver does the
// pacing; the software pacing is then never on the critical path.
class MidiOutScheduler {
public:
    // Longest message the ring carries inline. Channel voice is 3, the longest
    // useful realtime/common message is 3, MTC quarter-frame is 2; a fixed
    // inline array keeps the ring allocation-free, which is the whole point of
    // a ring. Anything longer (a sysex dump) is REFUSED by enqueue() and
    // counted in dropped() rather than truncated — a truncated sysex is worse
    // than a missing one.
    static constexpr std::size_t kMaxMessageBytes = 16;

    // Slots. 4096 x 32 B = 128 KB, and at the pump's 250 ms lookahead it is
    // three orders of magnitude more than a dense arrangement produces. It also
    // caps the sender's own pending list: the sender drains the ring eagerly,
    // so without that second cap a runaway producer would grow the list without
    // limit while the ring never looked full. Over the cap, the FURTHEST-FUTURE
    // messages are dropped and counted in dropped().
    static constexpr std::size_t kRingSlots = 4096;

    // A send later than this than its due time counts as late(). It is a
    // DIAGNOSTIC threshold, not a contract: WinMM's send-at-due-time is ±1 ms
    // by design and a loaded machine will exceed this occasionally.
    static constexpr std::int64_t kLateThresholdNs = 2'000'000;   // 2 ms

    explicit MidiOutScheduler(std::unique_ptr<MidiOutput> out);
    ~MidiOutScheduler();

    MidiOutScheduler(const MidiOutScheduler &)            = delete;
    MidiOutScheduler &operator=(const MidiOutScheduler &) = delete;

    // Opens the port if it is not open yet, then starts the sender thread.
    // Returns false if the port cannot be opened (the scheduler then stays
    // stopped and enqueue() is a no-op that counts drops).
    bool start(const std::string &portId = "default");

    // Stops and JOINS the sender thread. Idempotent; called by the destructor.
    // Anything still queued is DISCARDED — a queued future note-on that
    // escaped after the transport stopped is a stuck note, not a courtesy.
    void stop();

    bool running() const { return running_.load(std::memory_order_acquire); }

    // Queue one message for `dueHostTimeNs` (hostNowNs() domain; 0 or a past
    // time = send at the next opportunity). Returns false when the ring is
    // full or the message is too long / too short — both counted in dropped().
    bool enqueue(std::int64_t dueHostTimeNs,
                 const std::uint8_t *bytes, std::size_t size);

    // THE IMMEDIATE RING (proposal 21 L2, design D8) - MIDI-THRU.
    //
    // A second, dedicated SPSC ring whose producer is a MIDI INPUT DEVICE
    // THREAD and whose consumer is this scheduler's sender thread. It exists
    // because thru has no due time at all: the message is already late by the
    // driver's own latency, so it must leave at the next possible instant, and
    // the sender is woken immediately rather than at a deadline.
    //
    // It is NOT enqueue(). enqueue() is single-producer and that producer is
    // the app's main-thread pump (devices inv. 11); pushing from a device
    // thread would corrupt its head silently. Keeping the two rings separate
    // is what lets the two producers coexist with no lock on either path.
    //
    // ONE PRODUCER, and the caller guarantees it: MidiInFanout::setThru
    // refuses a second target for a port, and the app routes at most one input
    // port to a given scheduler. Returns false when the ring is full or the
    // message does not fit; both are counted in dropped().
    bool sendImmediate( const std::uint8_t *bytes, std::size_t size );
    std::uint64_t immediateSent() const
    { return immSent_.load( std::memory_order_relaxed ); }

    // Discard everything queued but not yet sent, and return once the sender
    // is idle. This is what a transport stop / locate wants: the queued future
    // describes a playhead that no longer exists. It does NOT push the queue
    // out — see stop() for why sending it would be wrong.
    void flush();

    // Sustain-off (CC64 = 0) then all-notes-off (CC123 = 0) on every channel
    // whose bit is set in `channelMask` (bit 0 = channel 1). Sent AHEAD of
    // anything still queued — panic() flushes first — and the call returns
    // once the bytes have left for the backend (bounded wait; if the sender
    // thread is not running they are sent inline on the caller's thread).
    void panic(std::uint16_t channelMask = 0xFFFF);

    MidiOutput *output() { return out_.get(); }
    const MidiOutput *output() const { return out_.get(); }

    // Diagnostics. Cheap atomics; a test and the log both read them.
    std::uint64_t sent() const    { return sent_.load(std::memory_order_relaxed); }
    std::uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
    std::uint64_t late() const    { return late_.load(std::memory_order_relaxed); }
    std::int64_t  maxLatenessNs() const
    {
        return maxLateness_.load(std::memory_order_relaxed);
    }
    void resetStats();

    // THE clock for every MIDI-out time: std::chrono::steady_clock in
    // nanoseconds. Monotonic (a wall-clock jump must not move a note) and the
    // same clock CaptureBackend stamps its blocks with, which is what lets the
    // testkit map a captured MIDI host time onto a project frame.
    static std::int64_t hostNowNs();

private:
    struct Slot {
        std::int64_t dueHostTimeNs = 0;
        std::uint8_t size          = 0;
        std::uint8_t bytes[kMaxMessageBytes] = {};
    };

    void threadMain();
    void drainRing(std::vector<Slot> &pending);
    // Drain the immediate ring and send every message NOW. Sender thread only.
    void drainImmediate();
    void wake();
    void sendNow(const Slot &s, std::int64_t now);

    // Sleep until `deadlineNs`, or until a producer wakes us / asks us to stop.
    // Windows and POSIX take different routes — see the .cc; `seen` is the wake
    // counter snapshot the condition_variable path needs and Windows ignores.
    void waitUntil(std::int64_t deadlineNs, std::uint64_t seen);
    void openWaitPrimitives();
    void closeWaitPrimitives();

    std::unique_ptr<MidiOutput> out_;

    // SPSC ring. head_ is written only by the producer, tail_ only by the
    // sender thread; both are read by the other side, hence the acquire/release
    // pairing rather than a mutex.
    std::vector<Slot>          ring_{ kRingSlots };
    std::atomic<std::size_t>   head_{ 0 };
    std::atomic<std::size_t>   tail_{ 0 };

    // The immediate (MIDI-thru) SPSC ring. Small on purpose: a thru message
    // that has been sitting here for 256 messages is not thru any more, and a
    // controller that outruns it is a drop worth counting rather than a queue
    // worth growing.
    static constexpr std::size_t kImmediateSlots = 256;
    std::vector<Slot>          immRing_{ kImmediateSlots };
    std::atomic<std::size_t>   immHead_{ 0 };   // the device thread
    std::atomic<std::size_t>   immTail_{ 0 };   // the sender thread
    std::atomic<std::uint64_t> immSent_{ 0 };

    std::thread                thread_;
    std::atomic<bool>          running_{ false };
    std::atomic<bool>          stopping_{ false };
    std::atomic<bool>          discard_{ false };   // flush() request
    std::atomic<bool>          idle_{ true };       // sender has nothing pending

    std::mutex                 wakeMutex_;
    std::condition_variable    wakeCv_;
    std::uint64_t              wakeSeq_ = 0;        // guarded by wakeMutex_

    // Windows only, and void* so no public header ever pulls in windows.h: a
    // HIGH-RESOLUTION waitable timer plus an auto-reset event. Measured, not
    // assumed — a condition_variable wait on this box rounds up to the 15.6 ms
    // system tick even with timeBeginPeriod(1) in force, which put every
    // message up to 15.4 ms late. See the .cc.
    void                      *winTimer_ = nullptr;
    void                      *winWake_  = nullptr;

    std::atomic<std::uint64_t> sent_{ 0 };
    std::atomic<std::uint64_t> dropped_{ 0 };
    std::atomic<std::uint64_t> late_{ 0 };
    std::atomic<std::int64_t>  maxLateness_{ 0 };
};

}  // namespace audio

#endif
