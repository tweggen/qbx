// tw/devices module test: the MIDI device layer (proposal 36 P7a).
//
// What it pins, and why each of these is here rather than in a qxa case:
//   1. The scheduler actually sends AT the due time. A qxa case can only see
//      the pump's end-to-end result; this measures the sender alone, against
//      the capture port's own record of when each message arrived.
//   2. stop()/join under a full ring — the shutdown path the orchestrator
//      review calls out (THREADING.md rule 1: a std::thread that is joined,
//      with no Qt anywhere near it).
//   3. CaptureBackend::frameAtHostTime over a SYNTHETIC log, which is the only
//      way to check the extrapolating ends and the interpolating middle
//      exactly; a real pump's log is never that tidy.
//   4. The env-var selection and the null port's never-fails contract.
//
// The timing check measures the MACHINE as much as the code (see twlog_test for
// the same caveat, and the CMake RUN_SERIAL that follows from it). It prints the
// measured maximum lateness so a failure is diagnosable rather than merely red.

#include "tw/devices/capture_backend.h"
#include "tw/devices/capture_midi.h"
#include "tw/devices/midi_input.h"
#include "tw/devices/midi_out_scheduler.h"
#include "tw/devices/midi_output.h"
#include "tw/devices/null_midi.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (cond) { printf("ok   %s\n", msg); }                             \
        else      { printf("FAIL %s\n", msg); ++failures; }                 \
    } while (0)

using namespace audio;
using namespace std::chrono_literals;

static void setEnv(const char *name, const char *value)
{
#if defined(_WIN32)
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1); else unsetenv(name);
#endif
}

// --- 1. capture round trip through the scheduler -----------------------------

static void testCaptureRoundTrip()
{
    printf("\n-- scheduler -> capture port round trip --\n");

    auto out = createMidiOutput("capture");
    CHECK(out && std::string(out->backendName()) == "capture",
          "createMidiOutput(\"capture\") yields the capture port");
    if (!out) return;

    auto *cap = dynamic_cast<CaptureMidiOutput *>(out.get());
    CHECK(cap == CaptureMidiOutput::active(),
          "the live capture port is reachable through the static accessor");
    CHECK(!out->supportsTimestamps(),
          "the capture port reports NO timestamp support (or it would record the "
          "handoff instant instead of the send instant, and measure nothing)");

    MidiOutScheduler sched(std::move(out));
    CHECK(sched.start("capture"), "the scheduler starts on the capture port");

    // 16 note-ons spread over 200 ms, first one 30 ms out so the enqueue itself
    // is never the thing being measured.
    const int kN = 16;
    const std::int64_t t0 = MidiOutScheduler::hostNowNs() + 30'000'000;
    std::vector<std::int64_t> due;
    for (int i = 0; i < kN; ++i) {
        const std::int64_t d = t0 + (std::int64_t) i * (200'000'000LL / kN);
        const std::uint8_t msg[3] = { 0x91, (std::uint8_t)(60 + i), 100 };
        due.push_back(d);
        if (!sched.enqueue(d, msg, 3)) { CHECK(false, "enqueue accepted"); return; }
    }

    // Wait for the last one, generously: this is a wait for a deadline that has
    // already been scheduled, not a race.
    for (int i = 0; i < 200 && cap->capturedCount() < (std::size_t) kN; ++i)
        std::this_thread::sleep_for(5ms);

    const auto events = cap->captured();
    CHECK(events.size() == (std::size_t) kN, "every enqueued message was sent exactly once");

    bool ordered = true;
    bool keysInOrder = true;
    for (std::size_t i = 0; i + 1 < events.size(); ++i)
        if (events[i].hostTimeNs > events[i + 1].hostTimeNs) ordered = false;
    for (std::size_t i = 0; i < events.size(); ++i) {
        if (events[i].bytes.size() != 3 || events[i].bytes[0] != 0x91 ||
            events[i].bytes[1] != (std::uint8_t)(60 + i))
            keysInOrder = false;
    }
    CHECK(ordered, "captured host times are non-decreasing");
    CHECK(keysInOrder, "messages arrived in due-time order, bytes intact");
    CHECK(!events.empty() && events[0].port == "capture",
          "each record carries the port it was sent on");

    std::int64_t worstNs = 0;
    for (std::size_t i = 0; i < events.size() && i < due.size(); ++i) {
        const std::int64_t err = events[i].hostTimeNs - due[i];
        worstNs = std::max(worstNs, err < 0 ? -err : err);
    }
    printf("     measured max |sent - due| = %.3f ms over %d messages"
           " (scheduler late-counter %llu, max lateness %.3f ms)\n",
           (double) worstNs / 1e6, kN, (unsigned long long) sched.late(),
           (double) sched.maxLatenessNs() / 1e6);
    // 5 ms, not the 1 ms the pacing aims for: this is a general-purpose OS and
    // the number above is a machine measurement. A regression that broke the
    // pacing outright (a lost wakeup, a 15.6 ms timer tick) lands far outside
    // this; ordinary scheduling noise does not.
    CHECK(worstNs <= 5'000'000, "every message landed within 5 ms of its due time");

    // panic(): channel 1 and channel 10 only.
    cap->clear();
    sched.panic((std::uint16_t)((1u << 0) | (1u << 9)));
    const auto panicEvents = cap->captured();
    CHECK(panicEvents.size() == 4, "panic(2 channels) sends 4 messages");
    bool panicShape = panicEvents.size() == 4;
    if (panicShape) {
        panicShape = panicEvents[0].bytes[0] == 0xB0 && panicEvents[0].bytes[1] == 64 &&
                     panicEvents[0].bytes[2] == 0 &&
                     panicEvents[1].bytes[0] == 0xB0 && panicEvents[1].bytes[1] == 123 &&
                     panicEvents[2].bytes[0] == 0xB9 && panicEvents[2].bytes[1] == 64 &&
                     panicEvents[3].bytes[0] == 0xB9 && panicEvents[3].bytes[1] == 123;
    }
    CHECK(panicShape, "panic sends sustain-off then all-notes-off, per channel, in order");

    // flush() DISCARDS: a queued future belongs to a playhead that stopped.
    cap->clear();
    const std::uint8_t far[3] = { 0x90, 60, 100 };
    sched.enqueue(MidiOutScheduler::hostNowNs() + 5'000'000'000LL, far, 3);
    sched.flush();
    std::this_thread::sleep_for(20ms);
    CHECK(cap->capturedCount() == 0, "flush() drops what was queued rather than sending it");

    sched.stop();
    CHECK(!sched.running(), "stop() leaves the scheduler stopped");
}

// --- 2. stop/join under load, and the ring's limits ---------------------------

static void testStopUnderLoad()
{
    printf("\n-- shutdown under a full queue --\n");

    auto out = createMidiOutput("capture");
    auto *cap = dynamic_cast<CaptureMidiOutput *>(out.get());
    MidiOutScheduler sched(std::move(out));
    CHECK(sched.start("capture"), "scheduler started");

    // Everything due in ten seconds: none of it may be sent, and stop() must
    // not wait for any of it.
    const std::int64_t farFuture = MidiOutScheduler::hostNowNs() + 10'000'000'000LL;
    const std::size_t  attempts  = MidiOutScheduler::kRingSlots * 2;
    std::size_t accepted = 0;
    std::size_t refused  = 0;
    const auto enqueueStart = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < attempts; ++i) {
        const std::uint8_t msg[3] = { 0x90, 64, 100 };
        if (sched.enqueue(farFuture, msg, 3)) ++accepted; else ++refused;
    }
    const auto enqueueMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - enqueueStart).count();
    CHECK(accepted + refused == attempts,
          "every enqueue answers immediately — accepted or refused, never blocking");
    CHECK(enqueueMs < 500, "and the whole overload was absorbed without stalling the producer");
    std::this_thread::sleep_for(50ms);      // let the sender apply its pending cap
    // Two bounds are in play and either may be the one that bites, depending on
    // how fast the sender drains: the ring refusing, or the sender's pending cap
    // discarding the furthest-future messages. What must hold is that the
    // overload is BOUNDED and COUNTED rather than growing without limit.
    printf("     %zu accepted, %zu refused at the ring, dropped() = %llu\n",
           accepted, refused, (unsigned long long) sched.dropped());
    CHECK(sched.dropped() >= attempts - MidiOutScheduler::kRingSlots,
          "the excess over one ring's worth is dropped and counted, never queued unbounded");

    const std::uint8_t huge[MidiOutScheduler::kMaxMessageBytes + 1] = { 0xF0 };
    CHECK(!sched.enqueue(0, huge, sizeof(huge)),
          "a message longer than a ring slot is refused, not truncated");

    const auto t0 = std::chrono::steady_clock::now();
    sched.stop();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
    printf("     stop() joined in %lld ms\n", (long long) ms);
    CHECK(ms < 1000, "stop() joins promptly and does not wait for queued due times");
    CHECK(cap->capturedCount() == 0, "nothing due in the future escaped after the stop");

    const std::uint8_t msg[3] = { 0x90, 64, 100 };
    CHECK(!sched.enqueue(0, msg, 3), "enqueue after stop() is refused");

    // Re-start must not resurrect the previous run's queue.
    CHECK(sched.start("capture"), "the scheduler restarts");
    std::this_thread::sleep_for(30ms);
    CHECK(cap->capturedCount() == 0, "a restart does not replay the discarded queue");
    sched.stop();
}

// --- 3. frameAtHostTime over a synthetic log ---------------------------------

static void testFrameAtHostTime()
{
    printf("\n-- CaptureBackend::frameAtHostTime --\n");

    using BS = CaptureBackend::BlockStamp;
    const double rate = 48000.0;

    CHECK(CaptureBackend::frameAtHostTime({}, 1000, rate) == -1,
          "an empty log answers -1 rather than a plausible-looking zero");

    // One stamp: the only slope available is the nominal one.
    std::vector<BS> single{ BS{ 1'000'000'000LL, 0 } };
    CHECK(CaptureBackend::frameAtHostTime(single, 1'500'000'000LL, rate) == 24000,
          "a single stamp extrapolates along the nominal rate (+0.5 s = +24000)");

    // Four blocks of 1024 frames at exactly 48 kHz: 21333333 ns apart.
    const std::int64_t base = 5'000'000'000LL;
    const std::int64_t step = 21'333'333LL;
    std::vector<BS> log;
    for (int i = 0; i < 4; ++i)
        log.push_back(BS{ base + (std::int64_t) i * step, (std::int64_t) i * 1024 });

    CHECK(CaptureBackend::frameAtHostTime(log, base, rate) == 0,
          "exactly on the first stamp -> its own frame");
    CHECK(CaptureBackend::frameAtHostTime(log, base + 2 * step, rate) == 2048,
          "exactly on an interior stamp -> its own frame");
    const std::int64_t mid =
        CaptureBackend::frameAtHostTime(log, base + step + step / 2, rate);
    CHECK(mid == 1536, "halfway between two stamps -> halfway between their frames");

    // Before the first and after the last: extrapolation along the end segment,
    // NOT a clamp. A MIDI event 10 ms before the first block is a real -480.
    const std::int64_t before = CaptureBackend::frameAtHostTime(log, base - step, rate);
    CHECK(before == -1024, "before the log, the first segment's slope continues");
    const std::int64_t after =
        CaptureBackend::frameAtHostTime(log, base + 4 * step, rate);
    CHECK(after == 4096, "after the log, the last segment's slope continues");

    // A degenerate segment (two stamps at the same instant) must not divide by
    // zero — it can happen if the pump is starved and two blocks are handed over
    // within one clock tick.
    std::vector<BS> flat{ BS{ 100, 0 }, BS{ 100, 1024 }, BS{ 200, 2048 } };
    const std::int64_t f = CaptureBackend::frameAtHostTime(flat, 100, rate);
    CHECK(f == 0 || f == 1024, "a zero-length segment answers one of its ends, not NaN");
}

// --- 4. backend selection, the null port, virtual ports ----------------------

static void testSelectionAndNull()
{
    printf("\n-- backend selection and the null port --\n");

    setEnv("SMARAGD_MIDI_BACKEND", "capture");
    {
        auto out = createMidiOutput();
        CHECK(out && std::string(out->backendName()) == "capture",
              "SMARAGD_MIDI_BACKEND=capture selects the capture port");
        auto in = createMidiInput();
        CHECK(in && std::string(in->backendName()) == "capture",
              "and the capture input");
    }
    setEnv("SMARAGD_MIDI_BACKEND", "NULL");        // case-insensitive, like the audio one
    {
        auto out = createMidiOutput();
        CHECK(out && std::string(out->backendName()) == "null",
              "SMARAGD_MIDI_BACKEND is case-insensitive");
    }
    setEnv("SMARAGD_MIDI_BACKEND", "nonsense");
    {
        auto out = createMidiOutput();
        CHECK(out != nullptr,
              "an unknown value warns and still yields the platform backend, never null-ptr");
    }
    setEnv("SMARAGD_MIDI_BACKEND", nullptr);
    {
        auto out = createMidiOutput();
        CHECK(out != nullptr, "unset -> the platform backend");
        printf("     platform backend on this build: %s\n", out->backendName());
    }

    // The null port never fails, so nothing above it needs a "MIDI is missing"
    // branch.
    NullMidiOutput nul;
    CHECK(nul.open("whatever") == 0 && nul.isOpen(), "null open() succeeds");
    const std::uint8_t msg[3] = { 0x90, 60, 100 };
    CHECK(nul.send(msg, 3) == 0 && nul.send(nullptr, 0) == 0,
          "null send() succeeds, even for nonsense");
    CHECK(nul.listPorts().empty(), "null exposes no ports");
    CHECK(nul.close() == 0 && !nul.isOpen(), "null close() succeeds");

    // A scheduler over the null port must run and shut down like any other:
    // this is the configuration a machine with no MIDI hardware gets.
    MidiOutScheduler nullSched(std::unique_ptr<MidiOutput>(new NullMidiOutput()));
    CHECK(nullSched.start(), "the scheduler runs over the null port");
    CHECK(nullSched.enqueue(0, msg, 3), "and accepts messages");
    nullSched.stop();
    CHECK(!nullSched.running(), "and stops");

#if defined(_WIN32)
    // Documented platform fact, asserted so it cannot silently start lying:
    // WinMM has no virtual ports (a loopback driver such as loopMIDI supplies
    // them, and they then appear as ordinary devices).
    auto win = createMidiOutput("winmm");
    CHECK(win && std::string(win->backendName()) == "winmm",
          "the WinMM backend is present on Windows");
    CHECK(win && !win->createVirtualPort("Smaragd"),
          "WinMM createVirtualPort() returns false — no OS concept of one");
    printf("     WinMM output ports on this machine: %d\n",
           win ? (int) win->listPorts().size() : -1);
#endif

    // The capture port CAN create one (it is virtual by construction), which is
    // what an Options page gating on this must see in a headless run.
    auto capOut = createMidiOutput("capture");
    CHECK(capOut && capOut->createVirtualPort("Smaragd Test"),
          "the capture port accepts createVirtualPort()");
    CHECK(capOut && capOut->listPorts().size() == 1,
          "and reports exactly one port");
}

// --- 5. the capture input --------------------------------------------------

static void testCaptureInput()
{
    printf("\n-- capture MIDI input --\n");

    auto in = createMidiInput("capture");
    auto *cin_ = dynamic_cast<CaptureMidiInput *>(in.get());
    CHECK(cin_ != nullptr && cin_ == CaptureMidiInput::active(),
          "the live capture input is reachable through the static accessor");
    if (!cin_) return;

    std::vector<std::uint8_t> got;
    std::int64_t             stamp = 0;
    cin_->setCallback([&](const std::uint8_t *b, std::size_t n, std::int64_t t) {
        got.assign(b, b + n);
        stamp = t;
    });

    const std::uint8_t msg[3] = { 0x90, 64, 127 };
    cin_->inject(msg, 3);
    CHECK(got.empty(), "a closed input delivers nothing");

    CHECK(in->open() == 0 && in->isOpen(), "the capture input opens");
    cin_->inject(msg, 3);
    CHECK(got.size() == 3 && got[1] == 64, "an injected message reaches the callback");
    CHECK(stamp > 0, "and carries a host time");
}

int main()
{
    testCaptureRoundTrip();
    testStopUnderLoad();
    testFrameAtHostTime();
    testSelectionAndNull();
    testCaptureInput();

    if (failures == 0) { printf("\nall MIDI device tests passed\n"); return 0; }
    printf("\n%d MIDI device test(s) FAILED\n", failures);
    return 1;
}
