// tw/schedule module test: CaptureRevalidator::retireObject() lifetime contract
// (proposal 19 crash fix). The reval queue holds BORROWED IRevalidatable*; an
// object destroyed while a worker still has a queued/in-flight job for it is a
// use-after-free (the observed crash: SCut::buildCapture_() locks a destroyed
// captureBuildMutex_ → std::mutex::lock() throws → terminate). retireObject(),
// called from the object's destructor, must (1) drop every queued job for it and
// (2) block until no worker is still processing it. This test verifies both,
// deterministically, without the app/UI.
#include "tw/schedule/capture_revalidator.h"
#include "tw/schedule/revalidatable.h"
#include "tw/schedule/capture_aspects.h"
#include "tw/pages/capture_page_pool.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <cstdio>

static int failures = 0;
#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (cond) { printf("ok   %s\n", msg); }                             \
        else      { printf("FAIL %s\n", msg); ++failures; }                 \
    } while (0)

using namespace std::chrono_literals;

// Stub revalidatable mirroring SCut's crash site: revalPrepPreview() does
// "buildCapture_"-like work — it locks its OWN mutex and sleeps (tens of ms) —
// then returns false so the generic preview render (which would need a real
// twComponent) is skipped. A worker touching this object after it is destroyed
// would be a UAF; retireObject() must make that impossible.
class SlowReval : public IRevalidatable {
public:
    mutable std::mutex m_;
    std::mutex buildMutex_;                        // stands in for captureBuildMutex_
    std::shared_ptr<CapturePageData> next_;
    std::atomic<int>  refs_{0};
    std::atomic<bool> inPrep_{false};
    std::atomic<bool> prepDone_{false};
    std::atomic<int>  prepCount_{0};
    int prepSleepMs_ = 60;

    std::mutex& revalMutex() const override { return m_; }
    void revalAddRef() override { ++refs_; }
    void revalRemoveRef() override { --refs_; }
    bool revalNeeded_nolock(uint32_t) const override { return true; }
    std::shared_ptr<CapturePageData> revalGetNextPage_nolock() const override { return next_; }
    void revalSetNextPage_nolock(std::shared_ptr<CapturePageData> p) override { next_ = p; }
    void revalSwapPages_nolock() override {}
    std::shared_ptr<twComponent> revalRootComponent() override { return nullptr; }
    void revalRecomputeMetadata(CapturePageData&) override {}
    void revalRecomputeExport(CapturePageData&) override {}
    bool revalPrepPreview() override {
        inPrep_.store(true);
        {
            std::lock_guard<std::mutex> lk(buildMutex_);   // the crash-site lock
            std::this_thread::sleep_for(std::chrono::milliseconds(prepSleepMs_));
        }
        ++prepCount_;
        prepDone_.store(true);
        inPrep_.store(false);
        return false;   // skip generic render (no real component in this stub)
    }
};

int main()
{
    // ---- Test 1: retireObject() DRAINS an in-flight job -------------------
    // Schedule a job, wait until a worker is inside revalPrepPreview(), then
    // retire. retireObject() must not return until that job has completed —
    // otherwise the subsequent destruction would race the worker.
    {
        CapturePagePool pool(16);
        CaptureRevalidator reval(&pool, 4);

        auto *s = new SlowReval();
        reval.scheduleRevalidation(s, Preview, 1);

        // Wait (bounded) until the worker is mid-prep.
        for (int i = 0; i < 2000 && !s->inPrep_.load(); ++i)
            std::this_thread::sleep_for(1ms);
        CHECK(s->inPrep_.load() && !s->prepDone_.load(),
              "a worker is in-flight (prep running) before we retire");

        reval.retireObject(s);
        CHECK(s->prepDone_.load(),
              "retireObject() blocked until the in-flight job finished");
        CHECK(s->refs_.load() == 0,
              "the drained job balanced its revalAddRef/revalRemoveRef");
        delete s;   // safe: no worker can be touching it anymore
    }

    // ---- Test 2: retireObject() DROPS queued jobs -------------------------
    // With workers paused, queue several jobs, retire, then resume: the dropped
    // jobs must never run.
    {
        CapturePagePool pool(16);
        CaptureRevalidator reval(&pool, 4);

        auto *s = new SlowReval();
        reval.pause();                              // no worker will dequeue
        reval.scheduleRevalidation(s, Preview, 1);
        reval.scheduleRevalidation(s, Preview, 1);
        reval.scheduleRevalidation(s, Preview, 1);

        reval.retireObject(s);                      // removes the queued jobs
        reval.resume();
        std::this_thread::sleep_for(80ms);          // ample time for a stray run

        CHECK(s->prepCount_.load() == 0,
              "retireObject() dropped all queued jobs (none ran after resume)");
        delete s;
    }

    // ---- Test 3: retireObject() on an object with NO jobs is a no-op ------
    {
        CapturePagePool pool(16);
        CaptureRevalidator reval(&pool, 2);
        auto *s = new SlowReval();
        reval.retireObject(s);                      // must return promptly
        CHECK(s->prepCount_.load() == 0, "retireObject() with no jobs is a safe no-op");
        delete s;
    }

    if (failures == 0) { printf("all schedule tests passed\n"); return 0; }
    printf("%d schedule test(s) FAILED\n", failures);
    return 1;
}
