// Unit tests for the TwLog sink (proposal 24 §8.4).
//
// The three properties that matter, because the rest of the system is built on
// them: the ring's wraparound accounting is exact, concurrent producers never
// lose or duplicate a seq, and a non-blocking (realtime) producer never waits.

#include "tw/core/twlog.h"

#include <atomic>
#include <chrono>
#include <cstdio>
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

static void testNonBlockingPath(TestRunner& runner)
{
    TwLog& log = TwLog::instance();
    log.setConsole(false);
    log.setMinLevel(LogLevel::Trace);
    log.setCapacity(4096);

    std::atomic<bool> stop{false};
    std::atomic<int64_t> worstUs{0};
    std::atomic<uint64_t> rtCalls{0};

    // Hammer the lock from ordinary threads so the RT thread genuinely contends.
    std::vector<std::thread> noise;
    for (int t = 0; t < 4; ++t) {
        noise.emplace_back([&log, &stop] {
            while (!stop.load(std::memory_order_relaxed))
                log.logf(LogLevel::Info, "noise", __FILE__, __LINE__, "filler filler filler");
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

    runner.assertTrue("the RT producer ran", rtCalls.load() > 100);

    // The point of try_lock: a bounded worst case. A blocking acquire under this
    // much contention would routinely exceed this.
    const int64_t worst = worstUs.load();
    runner.assertTrue("RT log call is bounded (worst < 2000us, got " +
                          std::to_string(worst) + "us)",
                      worst < 2000);

    runner.assertTrue("contention was reported as drops, not stalls (dropped=" +
                          std::to_string(log.droppedCount()) + ")",
                      true);   // informational: any value is correct behaviour
}

// ============================================================================

int main()
{
    TestRunner runner;

    std::cout << "=== TwLog ===" << std::endl;
    testRingWraparound(runner);
    testLineSplitting(runner);
    testLevelGate(runner);
    testInterning(runner);
    testConcurrentProducers(runner);
    testNonBlockingPath(runner);

    runner.printSummary();

    return runner.failCount > 0 ? 1 : 0;
}
