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
#include "tw/graph/twcomponent.h"
#include "tw/graph/twlatch.h"
#include "tw/graph/tw303aenv.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
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

// --- Dataflow stage 3 stubs -------------------------------------------------
// A minimal producer/consumer pair over the REAL latch plumbing, so the
// scheduler is exercised end-to-end: GraphSource renders a position ramp and
// exposes an output latch; GraphPass reads its input plug (readStreamingData
// -> copyData -> the bound-page seam) and passes it through.

static float rampVal(long long p) { return (float)((p % 977) + 1) / 1000.0f; }

class GraphSource : public twComponent {
public:
    explicit GraphSource(tw303aEnvironment &e) : twComponent(e) {}
    offset_t pos = 0;
    std::mutex logM;
    std::vector<offset_t> renderLog;   // start position of every real render

    bool isSeekable() const override { return true; }
    int seekTo(offset_t p) override { pos = p; return 0; }
    void reset() override { pos = 0; }
    length_t renderFrames(sample_t *out, length_t n, const sample_t *,
                          length_t, idx_t) override {
        { std::lock_guard<std::mutex> l(logM); renderLog.push_back(pos); }
        for (length_t i = 0; i < n; ++i) out[i] = rampVal((long long)(pos + i));
        pos += (offset_t)n;
        return n;
    }
    void createOutputLatches() override {
        pOutputLatches_[0] =
            std::make_shared<twStreamingLatch>(shared_from_this(), 0, 0);
    }
    idx_t getNInputs() const override { return 0; }
    idx_t getNOutputs() const override { return 1; }
    const char *getInputName(idx_t) const override { return nullptr; }
    const char *getOutputName(idx_t) const override { return "src"; }
    int renders() { std::lock_guard<std::mutex> l(logM); return (int)renderLog.size(); }
};

class GraphPass : public twComponent {
public:
    explicit GraphPass(tw303aEnvironment &e) : twComponent(e) {}
    offset_t pos = 0;
    std::atomic<int> renders{0};

    bool isSeekable() const override { return true; }
    int seekTo(offset_t p) override { pos = p; return 0; }
    void reset() override { pos = 0; }
    length_t renderFrames(sample_t *out, length_t n, const sample_t *,
                          length_t, idx_t) override {
        ++renders;
        std::shared_ptr<twLatchOutput> plug =
            pInputPlugs_.empty() ? nullptr : pInputPlugs_[0];
        length_t got = 0;
        if (plug)
            got = static_cast<twLatchStreamingOutput *>(plug.get())
                      ->readStreamingData(out, n);
        for (length_t i = got; i < n; ++i) out[i] = 0.f;
        pos += (offset_t)n;
        return n;
    }
    void createOutputLatches() override {
        pOutputLatches_[0] =
            std::make_shared<twStreamingLatch>(shared_from_this(), 0, 0);
    }
    idx_t getNInputs() const override { return 1; }
    idx_t getNOutputs() const override { return 1; }
    const char *getInputName(idx_t) const override { return "in"; }
    const char *getOutputName(idx_t) const override { return "out"; }
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

    // ---- Dataflow stage 3: the dependency-counting page scheduler ---------
    {
        tw303aEnvironment env;
        CapturePagePool pool(16);
        CaptureRevalidator reval(&pool, 4);
        const uint64_t CAP = twOutputPage::FRAME_CAPACITY;

        auto src = std::make_shared<GraphSource>(env);
        src->init();
        auto pass = std::make_shared<GraphPass>(env);
        pass->init();
        pass->setInput(0, src->linkOutput(0));

        // S3-1: chained demand — the source's nodes freeze first (dependency
        // counters), the consumer renders from the BOUND pages; render counts
        // are exact (the bound-serve seam prevented any double render).
        auto d1 = reval.requestGraphPages(pass, 0, 2);
        d1->wait();
        CHECK(d1->done(), "graph demand completes");
        CHECK(src->renders() == 2, "source rendered exactly once per page");
        CHECK(pass->renders.load() == 2, "consumer rendered exactly once per page");
        {
            std::lock_guard<std::mutex> l(src->logM);
            bool ordered = src->renderLog.size() == 2 &&
                           src->renderLog[0] == 0 &&
                           src->renderLog[1] == (offset_t)CAP;
            CHECK(ordered, "predecessor edge forces in-position-order rendering");
        }

        // Scheduled pages land in the components' own caches: the legacy pull
        // is now a cache hit — and the content is correct.
        auto p0 = pass->freezePage(0, nullptr, 0, (length_t)CAP,
                                   env.getSRate(), nullptr);
        CHECK(p0 && p0->validAspects != 0 &&
                  pass->renders.load() == 2 && src->renders() == 2,
              "scheduled pages serve the legacy pull as cache hits");
        CHECK(p0->samples[100] == rampVal(100), "scheduled content is correct");

        // S3-2: overlapping re-demands — dedup + cache hits, no re-renders.
        auto d2 = reval.requestGraphPages(pass, 0, 2);
        auto d3 = reval.requestGraphPages(pass, 0, 2);
        d2->wait();
        d3->wait();
        CHECK(src->renders() == 2 && pass->renders.load() == 2,
              "re-demanded pages are cache hits (no duplicate renders)");

        // S3-3: pause() gates the scheduler; resume() completes the demand.
        // With both components staled, exactly one re-render each.
        reval.pause();
        src->bumpContentEpoch();
        pass->bumpContentEpoch();
        auto d4 = reval.requestGraphPages(pass, 0, 1);
        std::this_thread::sleep_for(50ms);
        CHECK(!d4->done(), "paused scheduler executes nothing");
        reval.resume();
        d4->wait();
        CHECK(d4->done(), "resume completes the pending demand");
        CHECK(src->renders() == 3 && pass->renders.load() == 3,
              "stale pages re-render exactly once each after the epoch bump");

        // Stage 6 — completeness metrics (assert-first retirement of the
        // legacy pull): complete plans over current deps execute with ZERO
        // misses and ZERO retries.
        auto stats = reval.graphStats();
        CHECK(stats.nodesExecuted >= 6,
              "graph stats count the executed nodes");
        CHECK(stats.missPages == 0,
              "no bound-set misses across all scheduled renders");
        CHECK(stats.nodeRetries == 0,
              "no verify-at-publish retries across all scheduled renders");
    }

    if (failures == 0) { printf("all schedule tests passed\n"); return 0; }
    printf("%d schedule test(s) FAILED\n", failures);
    return 1;
}
