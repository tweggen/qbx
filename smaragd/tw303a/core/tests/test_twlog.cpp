// Unit tests for the TwLog sink (proposal 24 §8.4).
//
// The three properties that matter, because the rest of the system is built on
// them: the ring's wraparound accounting is exact, concurrent producers never
// lose or duplicate a seq, and a non-blocking (realtime) producer never waits.

#include "tw/core/twlog.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <iostream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

using tw::LogLevel;
using tw::LogRecord;
using tw::TwLog;

// ============================================================================
// Test Framework
// ============================================================================

class TestRunner {
public:
    int passCount = 0;
    int failCount = 0;

    void assertTrue(const std::string& testName, bool condition) {
        if (condition) {
            passCount++;
            std::cout << "PASS " << testName << std::endl;
        } else {
            failCount++;
            std::cout << "FAIL " << testName << std::endl;
        }
    }

    void assertEqualU(const std::string& testName, uint64_t actual, uint64_t expected) {
        if (actual == expected) {
            passCount++;
            std::cout << "PASS " << testName << std::endl;
        } else {
            failCount++;
            std::cout << "FAIL " << testName << " (got " << actual
                      << ", expected " << expected << ")" << std::endl;
        }
    }

    void printSummary() {
        std::cout << "\n" << passCount << " passed, " << failCount << " failed"
                  << std::endl;
    }
};

// ============================================================================
// Ring wraparound accounting
// ============================================================================

static void testRingWraparound(TestRunner& runner)
{
    TwLog& log = TwLog::instance();
    log.setConsole(false);
    log.setMinLevel(LogLevel::Trace);
    log.setCapacity(64);

    runner.assertEqualU("empty ring: nextSeq == 0", log.nextSeq(), 0);
    runner.assertEqualU("empty ring: firstSeq == 0", log.firstSeq(), 0);

    for (int i = 0; i < 40; ++i) log.logf(LogLevel::Info, "test", __FILE__, __LINE__, "record %d", i);

    runner.assertEqualU("under capacity: nextSeq", log.nextSeq(), 40);
    runner.assertEqualU("under capacity: firstSeq still 0", log.firstSeq(), 0);

    // Push past capacity: the oldest 40 must fall out.
    for (int i = 40; i < 104; ++i) log.logf(LogLevel::Info, "test", __FILE__, __LINE__, "record %d", i);

    runner.assertEqualU("over capacity: nextSeq", log.nextSeq(), 104);
    runner.assertEqualU("over capacity: firstSeq advanced", log.firstSeq(), 104 - 64);

    // snapshot() must clamp to what is still resident, not invent records.
    std::vector<LogRecord> out;
    size_t n = log.snapshot(0, log.nextSeq(), out);
    runner.assertEqualU("snapshot(0,next) clamps to capacity", n, 64);
    runner.assertTrue("snapshot starts at firstSeq",
                      !out.empty() && out.front().seq == 104 - 64);
    runner.assertTrue("snapshot ends at nextSeq-1",
                      !out.empty() && out.back().seq == 103);

    // Contents must match the seq, i.e. we read the slot we think we do.
    bool contentsOk = true;
    for (const LogRecord& r : out) {
        char expect[64];
        std::snprintf(expect, sizeof expect, "record %d", (int)r.seq);
        if (r.text != expect) { contentsOk = false; break; }
    }
    runner.assertTrue("snapshot contents match their seq", contentsOk);

    // A sub-range in the middle.
    n = log.snapshot(50, 60, out);
    runner.assertEqualU("sub-range count", n, 10);
    runner.assertTrue("sub-range start", !out.empty() && out.front().seq == 50);

    // A fully-evicted range yields nothing rather than garbage.
    n = log.snapshot(0, 10, out);
    runner.assertEqualU("fully evicted range is empty", n, 0);

    // An entirely-future range likewise.
    n = log.snapshot(500, 600, out);
    runner.assertEqualU("future range is empty", n, 0);
}

// ============================================================================
// One record per line
// ============================================================================

static void testLineSplitting(TestRunner& runner)
{
    TwLog& log = TwLog::instance();
    log.setCapacity(64);

    log.logf(LogLevel::Info, "test", __FILE__, __LINE__, "alpha\nbeta\ngamma");
    runner.assertEqualU("embedded newlines split into records", log.nextSeq(), 3);

    std::vector<LogRecord> out;
    log.snapshot(0, 3, out);
    runner.assertTrue("line 1", out.size() == 3 && out[0].text == "alpha");
    runner.assertTrue("line 2", out.size() == 3 && out[1].text == "beta");
    runner.assertTrue("line 3", out.size() == 3 && out[2].text == "gamma");

    log.setCapacity(64);
    log.logf(LogLevel::Info, "test", __FILE__, __LINE__, "trailing newline\n");
    log.snapshot(0, 1, out);
    runner.assertTrue("trailing newline stripped, not turned into a blank record",
                      log.nextSeq() == 1 && out.size() == 1 &&
                      out[0].text == "trailing newline");
}

// ============================================================================
// Level gating
// ============================================================================

static void testLevelGate(TestRunner& runner)
{
    TwLog& log = TwLog::instance();
    log.setCapacity(64);
    log.setMinLevel(LogLevel::Warn);

    log.logf(LogLevel::Error, "test", __FILE__, __LINE__, "error passes");
    log.logf(LogLevel::Warn,  "test", __FILE__, __LINE__, "warn passes");
    log.logf(LogLevel::Info,  "test", __FILE__, __LINE__, "info is dropped");
    log.logf(LogLevel::Debug, "test", __FILE__, __LINE__, "debug is dropped");

    runner.assertEqualU("minLevel gates at the call site", log.nextSeq(), 2);
    log.setMinLevel(LogLevel::Trace);
}

// ============================================================================
// Category and thread interning
// ============================================================================

static void testInterning(TestRunner& runner)
{
    TwLog& log = TwLog::instance();
    log.setCapacity(64);

    log.logf(LogLevel::Info, "devices",  __FILE__, __LINE__, "a");
    log.logf(LogLevel::Info, "schedule", __FILE__, __LINE__, "b");
    log.logf(LogLevel::Info, "devices",  __FILE__, __LINE__, "c");

    std::vector<LogRecord> out;
    log.snapshot(0, 3, out);
    runner.assertTrue("same category interns to the same id",
                      out.size() == 3 && out[0].catId == out[2].catId);
    runner.assertTrue("distinct categories get distinct ids",
                      out.size() == 3 && out[0].catId != out[1].catId);
    runner.assertTrue("category id resolves back to its name",
                      out.size() == 3 &&
                      std::string(TwLog::categoryName(out[0].catId)) == "devices");

    const uint16_t devicesId = out.empty() ? 0xFFFF : out[0].catId;

    // Same text through a different pointer must NOT create a second id --
    // otherwise the dock's category filter would list "devices" twice.
    std::string dyn = "devi";
    dyn += "ces";
    log.logf(LogLevel::Info, dyn.c_str(), __FILE__, __LINE__, "d");
    log.snapshot(3, 4, out);
    runner.assertTrue("category interned by text, not just pointer",
                      out.size() == 1 && out[0].catId == devicesId);

    // A recycled address must not inherit the dead string's category. Force the
    // case: let a heap string die, then log a DIFFERENT category and check it
    // did not get filed under the old name.
    uint16_t recycledId = 0xFFFF;
    {
        std::string tmp = "ephemeral-category";
        log.logf(LogLevel::Info, tmp.c_str(), __FILE__, __LINE__, "e");
        log.snapshot(4, 5, out);
        recycledId = out.empty() ? 0xFFFF : out[0].catId;
    }
    for (int i = 0; i < 32; ++i) {          // churn the allocator
        std::string reuse = "recycled-slot-XXXXX";
        log.logf(LogLevel::Info, reuse.c_str(), __FILE__, __LINE__, "r%d", i);
    }
    std::vector<LogRecord> tail;
    log.snapshot(log.nextSeq() - 1, log.nextSeq(), tail);
    runner.assertTrue("a recycled address does not inherit a dead category",
                      tail.size() == 1 && tail[0].catId != recycledId &&
                      std::string(TwLog::categoryName(tail[0].catId)) ==
                          "recycled-slot-XXXXX");
}

// ============================================================================
// Concurrency: no lost, no duplicated seq
// ============================================================================

static void testConcurrentProducers(TestRunner& runner)
{
    TwLog& log = TwLog::instance();
    log.setConsole(false);
    log.setMinLevel(LogLevel::Trace);

    const int kThreads = 8;
    const int kPerThread = 4000;
    const int kTotal = kThreads * kPerThread;

    // Ring larger than the total, so nothing is evicted and we can check the
    // full set rather than a window.
    log.setCapacity(kTotal + 1024);

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&log, t, kPerThread] {
            char name[32];
            std::snprintf(name, sizeof name, "producer-%d", t);
            TwLog::nameThread(name);
            for (int i = 0; i < kPerThread; ++i)
                log.logf(LogLevel::Info, "conc", __FILE__, __LINE__, "t%d i%d", t, i);
        });
    }
    for (std::thread& th : threads) th.join();

    runner.assertEqualU("every record landed", log.nextSeq(), (uint64_t)kTotal);

    std::vector<LogRecord> out;
    log.snapshot(0, log.nextSeq(), out);
    runner.assertEqualU("snapshot returns them all", out.size(), (size_t)kTotal);

    // Each seq appears exactly once and in order; no torn text.
    bool seqOk = true, textOk = true;
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i].seq != i) { seqOk = false; break; }
        const std::string& s = out[i].text;
        if (s.size() < 4 || s[0] != 't' || s.find(" i") == std::string::npos)
            { textOk = false; break; }
    }
    runner.assertTrue("seq is dense and strictly increasing", seqOk);
    runner.assertTrue("no torn/interleaved message text", textOk);

    // Every producer thread got its own interned slot.
    std::set<uint32_t> slots;
    for (const LogRecord& r : out) slots.insert(r.threadId);
    runner.assertEqualU("one thread slot per producer", slots.size(), (size_t)kThreads);
}

// ============================================================================
// The non-blocking (realtime) path must never wait
// ============================================================================
//
// INVARIANT 5 IS A BEHAVIOUR, NOT A LATENCY, AND IS NOW GATED AS ONE (QBX-193).
// This was one test that kept the worst single `logf` latency of an RT thread
// over 300 ms of five-way contention and asserted `worst < 2000us`. The intent
// was right -- the point of try_lock IS a bounded worst case -- but a worst
// case over 300 ms of five contending threads is dominated by OS SCHEDULING,
// not by the lock: one descheduling of the probe thread exceeds any microsecond
// bound with the design working perfectly. Measured on an unmodified tree, 60
// consecutive runs of this binary: 35 passed, 25 FAILED, the failures arriving
// in one contiguous block while the load average was 4.51 -- min 2820us,
// median 4989us, max 11452us, which is 5.7x the bound. It turned CI red on a
// PR whose entire diff was one paragraph of markdown, and again on a release
// gate (QBX-200), where the same run reported 211,295 records dropped to
// contention: the machine was saturated, which is the condition a wall-clock
// ceiling is least able to survive.
//
// Worse, the load-INSENSITIVE property sat beside it unasserted:
//
//     runner.assertTrue("contention was reported as drops, not stalls ...",
//                       true);   // informational: any value is correct
//
// `assertTrue(..., true)` asserts nothing, so the only real gate on "the RT
// path never waits" was a measurement of the scheduler. Relaxing the number
// would have left the design ungated; raising it makes the test meaningless at
// any load. So it is split in two, which is this project's standing rule --
// assert load-sensitive quantities as floors and the load-insensitive
// structure exactly:
//
//   1. testNonBlockingDropsRatherThanWaits -- the real gate, and it contains
//      no clock at all. The test HOLDS the sink's mutex, so try_lock cannot
//      succeed and the drop is a fact rather than a probability.
//   2. testNonBlockingUnderContention -- the five-thread scenario, kept
//      because it is the only place the two paths run concurrently for real,
//      with its load-sensitive quantities asserted as FLOORS and the worst-case
//      latency REPORTED rather than asserted.

// (1) THE DETERMINISTIC HALF, and the one that gates the design.
//
// Hold the lock, make exactly ONE non-blocking call, and the promise becomes
// observable without a clock: the call returns, the drop counter moves by
// exactly one, and the ring does not grow. Release, and the next call lands.
//
// WHY IT CANNOT HANG WHEN THE PROPERTY IS BROKEN: a build whose non-blocking
// path took a blocking acquire would wait on the held lock forever. So the call
// is made on its own thread and awaited with a bounded `wait_for`, and the
// guard is released before the future is joined. That timeout is NOT a latency
// bound -- it is a deadlock escape, and any value orders of magnitude above
// scheduling noise does the same job -- so this test has no tolerance that load
// can break. A blocking build fails it in about a second instead of timing the
// suite out.
static void testNonBlockingDropsRatherThanWaits(TestRunner& runner)
{
    TwLog& log = TwLog::instance();
    log.setConsole(false);
    log.setMinLevel(LogLevel::Trace);
    log.setCapacity(4096);

    // Deltas, never absolutes: the sink is a process-wide singleton and the
    // tests above have already written to this ring.
    const uint64_t seqBefore     = log.nextSeq();
    const uint64_t droppedBefore = log.droppedCount();

    std::future<void> call;
    bool     returnedWhileHeld = false;
    uint64_t droppedDelta      = 0;

    {
        // Inside this scope THIS thread may not log, and may not call
        // nextSeq()/firstSeq()/snapshot(): all of them take the very mutex the
        // guard holds, and std::mutex is not recursive. droppedCount() is
        // lock-free, so it is the one counter readable from in here.
        TwLog::SinkLockForTest held(log);

        call = std::async(std::launch::async, [&log] {
            TwLog::markNonBlocking();
            TwLog::nameThread("rt-probe");
            log.logf(LogLevel::Warn, "rt", __FILE__, __LINE__, "one rt record");
        });

        returnedWhileHeld =
            call.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
        if (returnedWhileHeld)
            droppedDelta = log.droppedCount() - droppedBefore;
    }

    // The lock is free again, so a call that BLOCKED (a broken build) completes
    // here instead of deadlocking the test process.
    call.wait();

    runner.assertTrue("a non-blocking logf RETURNS while the lock is held",
                      returnedWhileHeld);
    runner.assertEqualU("...counting exactly one drop", droppedDelta, 1);
    runner.assertEqualU("...and the dropped record never reached the ring",
                        log.nextSeq(), seqBefore);
    // A build that blocks rather than drops fails all three, and all three name
    // the same cause. That is deliberate: the pass/fail counts stay comparable
    // between a good and a broken run, which a skip would not do.

    // THE OTHER HALF OF THE PROMISE: dropping is what CONTENTION costs, not
    // what the path always does. With the lock free, the same call must land --
    // otherwise "never waits" could be satisfied by never logging at all.
    const uint64_t droppedAtRelease = log.droppedCount();
    std::thread after([&log] {
        TwLog::markNonBlocking();
        TwLog::nameThread("rt-probe-2");
        log.logf(LogLevel::Warn, "rt", __FILE__, __LINE__, "one rt record");
    });
    after.join();

    runner.assertEqualU("with the lock free the same call LANDS",
                        log.nextSeq(), seqBefore + 1);
    runner.assertEqualU("...and counts no drop",
                        log.droppedCount(), droppedAtRelease);
}

// (2) THE CONTENTION HALF. It asserts the BEHAVIOUR and reports the numbers.
//
// Four noise threads hammer the lock while one non-blocking thread logs as fast
// as it can for 300 ms. Nothing here is a tolerance on time, and -- measured,
// see below -- nothing here is a throughput floor either.
//
// WHY THERE IS NO THROUGHPUT FLOOR, THOUGH QBX-193 ASKED FOR ONE. The ticket
// proposed raising the old `rtCalls > 100` to "something a BLOCKING
// implementation could not reach". No such number exists, because a
// non-blocking producer's advantage requires it to be SCHEDULED: starve it of
// CPU and its throughput collapses towards the blocking threads it competes
// with, while the design is working perfectly. Measured on the 12-core dev box,
// same binary, only the emit path changed:
//
//   non-blocking (try_lock, shipped), idle         : 626,505-769,873 calls
//   blocking acquire (sabotage S1), idle           : 118,015-128,879 calls
//   non-blocking (shipped), 36 spinners on 12 cores: 134,259-252,420 calls
//
// The third row OVERLAPS the second, so an absolute floor between them does not
// exist. A RATIO against one noise thread in the same run was tried next and
// fails for the same reason: 4.05-5.60x idle, but 1.18-2.2x under that load, so
// a 2x floor failed 23 runs in 60. (For scale, the OLD 2000us ceiling failed
// 60 of those same 60.) A floor low enough to be safe everywhere is one a
// blocking build also clears, which gates nothing.
//
// So the load-sensitive quantities are PRINTED and the load-INSENSITIVE
// behaviour is asserted: a non-blocking producer that loses the race DROPS, and
// a blocking one never drops at all (sabotage S1 reports exactly zero). The
// deterministic test above is where the design is really gated; this one proves
// the two paths survive each other for real and that the scenario contends.

// The scenario has to have RUN. Deliberately far below anything either
// implementation reaches, because it is not a performance claim: 1000 calls
// over 300 ms is 3.3 per millisecond, and a box that cannot manage that has
// told us something other than whether try_lock works.
static const uint64_t kScenarioRanFloor = 1000;

static void testNonBlockingUnderContention(TestRunner& runner)
{
    TwLog& log = TwLog::instance();
    log.setConsole(false);
    log.setMinLevel(LogLevel::Trace);
    log.setCapacity(4096);

    const uint64_t droppedBefore = log.droppedCount();

    std::atomic<bool> stop{false};
    std::atomic<int64_t> worstUs{0};
    std::atomic<uint64_t> rtCalls{0};
    std::atomic<uint64_t> noiseCalls{0};

    // Hammer the lock from ordinary threads so the RT thread genuinely contends.
    std::vector<std::thread> noise;
    for (int t = 0; t < 4; ++t) {
        noise.emplace_back([&log, &stop, &noiseCalls] {
            while (!stop.load(std::memory_order_relaxed)) {
                log.logf(LogLevel::Info, "noise", __FILE__, __LINE__, "filler filler filler");
                noiseCalls.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    std::thread rt([&log, &stop, &worstUs, &rtCalls] {
        TwLog::markNonBlocking();
        TwLog::nameThread("audio-rt");
        while (!stop.load(std::memory_order_relaxed)) {
            const auto t0 = std::chrono::steady_clock::now();
            log.logf(LogLevel::Warn, "rt", __FILE__, __LINE__, "rt tick %llu",
                     (unsigned long long)rtCalls.load(std::memory_order_relaxed));
            const auto dt = std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - t0).count();
            if (dt > worstUs.load(std::memory_order_relaxed))
                worstUs.store(dt, std::memory_order_relaxed);
            rtCalls.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    stop.store(true, std::memory_order_relaxed);
    rt.join();
    for (std::thread& th : noise) th.join();

    const uint64_t calls    = rtCalls.load();
    const uint64_t perNoise = noiseCalls.load() / 4;
    const uint64_t dropped  = log.droppedCount() - droppedBefore;

    // REPORTED, NOT ASSERTED -- all three. The worst case is the number the old
    // bound gated on, and what it is honestly good for is telling a reader how
    // loaded the machine was. The throughput and the ratio are here so that a
    // future reader can repeat the measurement in the comment above rather than
    // trust it. None of them costs a pass count.
    std::cout << "INFO rt=" << calls << " calls vs " << perNoise
              << " per blocking producer (ratio "
              << (perNoise ? (double)calls / (double)perNoise : 0.0) << "x), "
              << dropped << " dropped, worst single RT logf " << worstUs.load()
              << "us -- all REPORTED, none asserted: every one of them is a"
                 " property of the scheduler as much as of the code (QBX-193)"
              << std::endl;

    runner.assertTrue("the contention scenario ran (rt calls=" +
                          std::to_string(calls) + " >= " +
                          std::to_string(kScenarioRanFloor) + ")",
                      calls >= kScenarioRanFloor);

    // THE LOAD-INSENSITIVE BEHAVIOUR, and the only assertion here that gates
    // the design: under real contention a non-blocking producer loses the race
    // sometimes, and when it does it DROPS. A build that waits instead reports
    // exactly zero drops however loaded the machine is, so this bites sabotage
    // S1 on every host. It also keeps the scenario from going vacuous -- a run
    // in which the RT thread never once lost the race would prove nothing.
    runner.assertTrue("contention was reported as drops, not stalls (dropped=" +
                          std::to_string(dropped) + ")",
                      dropped > 0);
}

// ============================================================================
// The sink is IMMORTAL, and shutdown() is not a destructor
// ============================================================================
//
// A late record -- from a background thread that outlives main, or from an
// atexit handler running after static destruction -- must never be able to
// touch a destroyed mutex. It used to: the singleton was a function-local
// `static TwLog inst`, constructed at the first log call (inside main) and
// therefore destroyed BEFORE namespace-scope statics that own threads. The
// plugin registry is one of those; its scan thread logged into the corpse, the
// std::system_error escaped, and the process abort()ed after printing PASS
// (plan/STATE.md 2026-08-16).
//
// Two halves are asserted here. The cheap half: shutdown() is idempotent and
// logging after it still works, from this thread and from a fresh one. The
// sharp half is the atexit handler registered at the very top of main(), BEFORE
// anything in this process has called TwLog::instance() -- destructor and
// atexit registrations run in one LIFO order, so a handler registered first
// runs LAST, after the sink's destructor would have run. With an immortal sink
// it is simply a log call; with the old one it was undefined behaviour that
// reliably terminated this binary.
static void testShutdownIsSafe(TestRunner& runner)
{
    std::cout << "\n-- shutdown() is safe, and late records do not crash --"
              << std::endl;

    TwLog& log = TwLog::instance();

    // No file sink was ever started here, so this is the no-op path.
    log.shutdown();
    log.shutdown();   // idempotent

    const uint64_t before = log.nextSeq();
    TW_LOGI("shutdown", "logged after shutdown, from the main thread");

    std::atomic<bool> threadOk{false};
    std::thread late([&threadOk] {
        TW_LOGI("shutdown", "logged after shutdown, from a thread started after it");
        threadOk.store(true, std::memory_order_relaxed);
    });
    late.join();

    runner.assertTrue("a thread started after shutdown() can still log",
                      threadOk.load(std::memory_order_relaxed));
    runner.assertTrue("records emitted after shutdown() still reach the ring",
                      log.nextSeq() == before + 2);

    // And the sink is still usable afterwards: shutting down does not poison it.
    log.shutdown();
    runner.assertTrue("shutdown() after a late record is still safe", true);
}

// ============================================================================

int main()
{
    // Registered FIRST, before any TwLog::instance() call in this process, so
    // it runs LAST -- after the point at which a mortal sink would already be
    // destroyed. See testShutdownIsSafe above.
    std::atexit([] {
        TW_LOGI("atexit", "logged from an atexit handler, after static destruction");
    });

    TestRunner runner;

    std::cout << "=== TwLog ===" << std::endl;
    testRingWraparound(runner);
    testLineSplitting(runner);
    testLevelGate(runner);
    testInterning(runner);
    testConcurrentProducers(runner);
    testNonBlockingDropsRatherThanWaits(runner);
    testNonBlockingUnderContention(runner);
    testShutdownIsSafe(runner);

    runner.printSummary();

    return runner.failCount > 0 ? 1 : 0;
}
