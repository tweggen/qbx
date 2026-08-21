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
#include <cstdlib>

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
    // Render gate (the playback_test ToneComponent::renderDelayMs idea, made
    // deterministic instead of timed): a test can hold this component inside
    // renderFrames and act while its CONSUMER's node is planned but not yet
    // executed — the exact window a mid-render edit lands in.
    std::atomic<bool> inRender{false};
    std::atomic<bool> gateOpen{true};

    bool isSeekable() const override { return true; }
    int seekTo(offset_t p) override { pos = p; return 0; }
    void reset() override { pos = 0; }
    length_t renderFrames(sample_t *out, length_t n, const sample_t *,
                          length_t, idx_t) override {
        inRender.store(true);
        for (int i = 0; i < 5000 && !gateOpen.load(); ++i)
            std::this_thread::sleep_for(1ms);
        inRender.store(false);
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

// A source that also counts how often it was PLANNED. retireComponentNodes()
// promises that a later demand plans FRESH rather than adopting the node it
// just retired, and the plan count is the only place that is visible: a
// re-planned page renders identically to an adopted one.
class CountingSource : public GraphSource {
public:
    explicit CountingSource(tw303aEnvironment &e) : GraphSource(e) {}
    std::atomic<int> plans{0};
    twPagePlan planPage(offset_t pageStart) override {
        ++plans;
        return GraphSource::planPage(pageStart);
    }
};

// Bounded wait for a demand, so a broken retirement fails the test instead of
// hanging it (a hang is a failure too, but a named one is a better bug report).
static bool waitDemand(const std::shared_ptr<CaptureRevalidator::GraphDemand> &d,
                       int ms = 5000)
{
    for (int i = 0; i < ms && !d->done(); ++i)
        std::this_thread::sleep_for(1ms);
    return d->done();
}

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

    // ---- Test 4: shutdown() must return while the pool is PAUSED ----------
    // The reval lane is the one shutdown() used to leave standing, and the
    // worker exit test used to require every queue empty. Paused + one queued
    // job = each worker spinning between the wait predicate (true the moment
    // shutdown_ is set) and the paused `continue` below it, forever, while
    // shutdown()'s join() waits for threads that will never return. Observed
    // as a hung app in ~SProject on File -> Open.
    {
        auto *pool  = new CapturePagePool(16);
        auto *reval = new CaptureRevalidator(pool, 4);
        auto *s     = new SlowReval();

        reval->pause();                      // no worker will dequeue
        reval->scheduleRevalidation(s, Preview, 1);

        std::atomic<bool> done{false};
        std::thread t([&]() { delete reval; done.store(true); });
        // WALL CLOCK, not an iteration count: when this regresses the four
        // workers spin at 100 % CPU, a 1 ms sleep on Windows becomes ~15 ms,
        // and a 5000-iteration loop is a 78-second watchdog that CTest kills
        // first — the named failure would never be printed.
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!done.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
        CHECK(done.load(),
              "shutdown() returns while the pool is paused with a queued job");
        if (!done.load()) {
            // The workers are spinning and the join will never return, so
            // there is nothing to join here either: report the named failure
            // instead of burning the whole CTest timeout on a hang.
            printf("%d test(s) failed\n", failures);
            fflush(stdout);
            std::_Exit(1);
        }
        t.join();
        CHECK(s->prepCount_.load() == 0,
              "the queued job was dropped by shutdown(), not run");
        // refs_ stays 1 on purpose: a dropped job is NOT unpinned, exactly as
        // retireObject() documents (the last unpin re-arms a deleteLater() on
        // an object that is already being destroyed).
        delete s;
        delete pool;
    }

    // ---- Test 5: after shutdown(), scheduling refuses and takes no pin -----
    {
        CapturePagePool pool(16);
        CaptureRevalidator reval(&pool, 2);
        auto *s = new SlowReval();

        reval.shutdown();                    // idempotent: the dtor repeats it
        reval.scheduleRevalidation(s, Preview, 1);

        CHECK(s->refs_.load() == 0,
              "scheduleRevalidation() after shutdown() takes no pin");
        CHECK(reval.jobsQueued() == 0,
              "scheduleRevalidation() after shutdown() queues nothing");
        std::this_thread::sleep_for(20ms);
        CHECK(s->prepCount_.load() == 0,
              "scheduleRevalidation() after shutdown() runs nothing");
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
        CHECK(p0->channelPtr(0)[100] == rampVal(100), "scheduled content is correct");

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

    // ---- Verify-at-publish: SELF staleness (an outdated PLAN) -------------
    // A node's plan — which deps exist, and for a track mix which clip resolves
    // to which component at which position — is captured by planPage() at
    // expansion time. An edit landing between then and the node's execution
    // means the node renders the PRE-EDIT arrangement. It must not be left
    // readable as current: a page stamped with the epoch its render happened to
    // read looks fresh to every consumer, and the moved clip stays audible at
    // its old position until something unrelated invalidates it.
    {
        tw303aEnvironment env;
        CapturePagePool pool(16);
        CaptureRevalidator reval(&pool, 4);

        auto src = std::make_shared<GraphSource>(env);
        src->init();
        auto pass = std::make_shared<GraphPass>(env);
        pass->init();
        pass->setInput(0, src->linkOutput(0));

        // Hold the SOURCE inside its render. The consumer's node is planned
        // (requestGraphPages expanded the whole DAG up front) but blocked on
        // the dependency counter, so it cannot have executed yet.
        src->gateOpen.store(false);
        auto d = reval.requestGraphPages(pass, 0, 1);
        bool entered = false;
        for (int i = 0; i < 5000 && !(entered = src->inRender.load()); ++i)
            std::this_thread::sleep_for(1ms);
        CHECK(entered, "the dep is mid-render while the consumer node waits");

        // THE EDIT: the consumer's own content changes after its plan was made.
        const uint64_t planEpoch = pass->contentEpochNow();
        pass->bumpContentEpoch();
        src->gateOpen.store(true);
        d->wait();

        auto p = pass->getPageIfExists(0);
        CHECK(p && p->validAspects != 0,
              "the outdated-plan page is still PUBLISHED (no dropout: the RT "
              "stale-page fallback serves it while the re-plan runs)");
        CHECK(p && p->contentEpoch.load() < pass->contentEpochNow(),
              "a page rendered from an outdated plan does NOT read as current");
        CHECK(pass->contentEpochNow() > planEpoch + 1,
              "publishing it re-staled the position, so the epoch-scoped "
              "readahead supersession re-demands and re-plans it");

        auto st = reval.graphStats();
        CHECK(st.selfStale == 1, "exactly one self-stale publish was recorded");
        CHECK(st.nodeRetries == 0,
              "an outdated PLAN is not retried: the same dep set would rebuild "
              "the same wrong structure");
        CHECK(src->renders() == 1,
              "and the dep was rendered exactly once (no extra work)");
    }

    // ---- Converse: no edit ⇒ published current, nothing over-invalidated --
    // Guards the other direction. Self-staleness that fired on its own bumps
    // would re-stale every page it published, and the readahead would re-demand
    // them forever.
    {
        tw303aEnvironment env;
        CapturePagePool pool(16);
        CaptureRevalidator reval(&pool, 4);

        auto src = std::make_shared<GraphSource>(env);
        src->init();
        auto pass = std::make_shared<GraphPass>(env);
        pass->init();
        pass->setInput(0, src->linkOutput(0));

        const uint64_t before = pass->contentEpochNow();
        auto d = reval.requestGraphPages(pass, 0, 3);
        d->wait();

        bool allCurrent = true;
        for (int i = 0; i < 3; ++i) {
            auto p = pass->getPageIfExists((offset_t)i * twOutputPage::FRAME_CAPACITY);
            if (!p || p->validAspects == 0 ||
                p->contentEpoch.load() < pass->contentEpochNow())
                allCurrent = false;
        }
        CHECK(allCurrent, "with no edit, every published page reads as current");
        CHECK(pass->contentEpochNow() == before,
              "no edit ⇒ the scheduler bumped nothing (no over-invalidation)");

        auto st = reval.graphStats();
        CHECK(st.selfStale == 0, "no self-stale publishes without an edit");
        CHECK(st.nodeRetries == 0 && st.missPages == 0,
              "and no retries or bound-set misses");
        CHECK(src->renders() == 3 && pass->renders.load() == 3,
              "each page rendered exactly once");
    }

    // ---- Proposal 21 L0 / design §5: retireComponentNodes() ---------------
    //
    // The live lane takes a track's processors out of the frozen graph, so the
    // nodes already planned for those components must stop — without pause(),
    // which would drain import-time analysis and stop the graph everywhere.

    // R-1: QUEUED nodes are dropped and NEVER execute; their demands complete
    // as "not produced"; a re-demand plans fresh.
    {
        tw303aEnvironment env;
        CapturePagePool pool(16);
        CaptureRevalidator reval(&pool, 4);

        auto src = std::make_shared<CountingSource>(env);
        src->init();

        reval.pause();                       // nothing will be dequeued
        auto d = reval.requestGraphPages(src, 0, 3);
        const int plansAfterDemand = src->plans.load();
        CHECK(plansAfterDemand == 3, "three pages planned");

        reval.retireComponentNodes({ (const twComponent *) src.get() });
        reval.resume();

        CHECK(waitDemand(d), "the demand completes after its nodes were retired");
        CHECK(d->notProduced() == 3,
              "all three root pages completed as NOT PRODUCED (the consumer's "
              "miss signal), rather than waiting for a page nobody will make");
        std::this_thread::sleep_for(60ms);   // ample time for a stray execution
        CHECK(src->renders() == 0,
              "no node of the retired component ever executed");

        // A later demand re-PLANS: the dedup entries went with the nodes.
        auto d2 = reval.requestGraphPages(src, 0, 3);
        CHECK(waitDemand(d2), "the re-demand completes");
        CHECK(src->plans.load() == plansAfterDemand + 3,
              "a demand after retirement plans FRESH (dedup entries removed)");
        CHECK(d2->notProduced() == 0 && src->renders() == 3,
              "and it really renders this time");
    }

    // R-2: a RUNNING node is WAITED FOR — the call does not return while a
    // worker is still inside the component.
    {
        tw303aEnvironment env;
        CapturePagePool pool(16);
        CaptureRevalidator reval(&pool, 4);

        auto src = std::make_shared<GraphSource>(env);
        src->init();
        src->gateOpen.store(false);          // hold the render open

        auto d = reval.requestGraphPages(src, 0, 1);
        for (int i = 0; i < 5000 && !src->inRender.load(); ++i)
            std::this_thread::sleep_for(1ms);
        CHECK(src->inRender.load(), "a worker is inside the component's render");

        std::thread opener([&src] {
            std::this_thread::sleep_for(120ms);
            src->gateOpen.store(true);
        });
        const auto t0 = std::chrono::steady_clock::now();
        reval.retireComponentNodes({ (const twComponent *) src.get() });
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - t0).count();
        opener.join();

        CHECK(elapsed >= 100,
              "retireComponentNodes() blocked until the running node finished");
        CHECK(!src->inRender.load() && src->renders() == 1,
              "the running node completed rather than being abandoned");
        CHECK(waitDemand(d) && d->notProduced() == 0,
              "its demand completed WITH a page (it had already been produced)");
    }

    // R-3: other components are untouched — same revalidator, same instant.
    {
        tw303aEnvironment env;
        CapturePagePool pool(16);
        CaptureRevalidator reval(&pool, 4);

        auto victim = std::make_shared<CountingSource>(env);
        victim->init();
        auto bystander = std::make_shared<CountingSource>(env);
        bystander->init();

        reval.pause();
        auto dv = reval.requestGraphPages(victim, 0, 3);
        auto db = reval.requestGraphPages(bystander, 0, 3);

        reval.retireComponentNodes({ (const twComponent *) victim.get() });
        reval.resume();

        CHECK(waitDemand(dv) && waitDemand(db), "both demands complete");
        CHECK(db->notProduced() == 0 && bystander->renders() == 3,
              "the OTHER component's nodes ran normally and produced every page");
        CHECK(dv->notProduced() == 3 && victim->renders() == 0,
              "while the retired component produced nothing");
        CHECK(bystander->plans.load() == 3,
              "and the bystander was not re-planned");
    }

    // R-4: a consumer whose input was retired sees a MISS, not a wait. The
    // dependent node loses the edge, becomes runnable, and renders with that
    // input unbound (design §5: "in-flight dependents of old plans see a miss").
    {
        tw303aEnvironment env;
        CapturePagePool pool(16);
        CaptureRevalidator reval(&pool, 4);

        auto src = std::make_shared<GraphSource>(env);
        src->init();
        auto pass = std::make_shared<GraphPass>(env);
        pass->init();
        pass->setInput(0, src->linkOutput(0));

        reval.pause();
        auto d = reval.requestGraphPages(pass, 0, 2);
        reval.retireComponentNodes({ (const twComponent *) src.get() });
        reval.resume();

        CHECK(waitDemand(d), "the consumer's demand still completes");
        CHECK(d->notProduced() == 0,
              "the consumer's OWN pages were produced (only its input was retired)");
        CHECK(pass->renders.load() >= 2,
              "the dependent ran rather than waiting on a node that will never come");
        // And it saw the retirement AS A MISS: its plan wanted the retired
        // component's page, the bound set did not have it, which is exactly the
        // signal verify-at-publish already counts. (That also costs the node its
        // ONE bounded retry, which is why the render count above is >= and not
        // ==; content stays correct through the legacy fallback inside the
        // render.)
        CHECK(reval.graphStats().missPages > 0,
              "the retired input shows up as a bound-set MISS, not as a wait");
    }

    // R-5: 100 randomized interleavings. The retirement lands at an arbitrary
    // point in the run — before the first dequeue, mid-render, after some pages
    // are done — and the invariant is the same every time: nothing of that
    // component executes after the call returns, and the demand completes.
    {
        int badExec = 0, badDone = 0, badCount = 0;
        unsigned seed = 12345u;
        for (int iter = 0; iter < 100; ++iter) {
            tw303aEnvironment env;
            CapturePagePool pool(16);
            CaptureRevalidator reval(&pool, (iter % 4) + 1);

            auto src = std::make_shared<GraphSource>(env);
            src->init();

            auto d = reval.requestGraphPages(src, 0, 4);

            // xorshift: deterministic across runs, arbitrary against the
            // scheduler. 0..3 ms covers "before anything ran" through "most of
            // it ran" for a 4-page demand of this stub.
            seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
            const int delayUs = (int)(seed % 3000);
            std::this_thread::sleep_for(std::chrono::microseconds(delayUs));

            reval.retireComponentNodes({ (const twComponent *) src.get() });
            const int atReturn = src->renders();

            if (!waitDemand(d)) ++badDone;
            std::this_thread::sleep_for(20ms);
            if (src->renders() != atReturn) ++badExec;
            // Every root page is accounted for exactly once, either way.
            if (d->notProduced() < 0 || d->notProduced() > 4) ++badCount;
        }
        CHECK(badExec == 0,
              "100 randomized interleavings: no node executed after the call returned");
        CHECK(badDone == 0,
              "100 randomized interleavings: every demand completed (no hang)");
        CHECK(badCount == 0,
              "100 randomized interleavings: the not-produced count stays in range");
    }

    if (failures == 0) { printf("all schedule tests passed\n"); return 0; }
    printf("%d schedule test(s) FAILED\n", failures);
    return 1;
}
