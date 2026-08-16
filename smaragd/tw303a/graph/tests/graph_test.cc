// tw/graph module test.
//
// Two things, neither of which had a gate before:
//
//   1. releaseOldPages()'s RETENTION BOUNDARY. It compared
//      `it->first + twOutputPage::PAGE_SIZE < keepAfterPos` — PAGE_SIZE is the
//      page's size in BYTES (262144) while keepAfterPos and it->first are FRAME
//      positions, so the window was 4x too generous (262144 frames instead of
//      65536). The boundary is now pinned frame-exactly from both sides, so a
//      future edit that re-introduces a unit confusion fails here rather than
//      turning up as a memory figure nobody can explain.
//
//   2. PAGE-MEMORY ACCOUNTING. Resident pages and bytes, globally
//      (tw::pages::PageAccounting, which rides the page's own lifetime) and per
//      component (twComponent::pageStats / componentPageStats). It is the
//      instrument any future memory claim about this engine has to rest on, so
//      its own arithmetic is checked here against pages whose count the test
//      controls exactly.
#include "tw/graph/twcomponent.h"
#include "tw/graph/twlatch.h"
#include "tw/graph/tw303aenv.h"
#include "tw/pages/tw_output_page.h"
#include "tw/pages/tw_page_accounting.h"

#include <cstdio>
#include <memory>
#include <string>

static int failures = 0;
#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (cond) { printf("ok   %s\n", msg); }                             \
        else      { printf("FAIL %s\n", msg); ++failures; }                 \
    } while (0)

// The smallest thing that is a twComponent: it never renders, because nothing
// here is about rendering. getOrAllocatePage() is the page cache's own door and
// is all these tests need.
class BareComponent : public twComponent {
public:
    explicit BareComponent(tw303aEnvironment &e) : twComponent(e) {}
    void reset() override {}
    void createOutputLatches() override {
        pOutputLatches_[0] =
            std::make_shared<twStreamingLatch>(shared_from_this(), 0, 0);
    }
    idx_t getNInputs() const override { return 0; }
    idx_t getNOutputs() const override { return 1; }
    const char *getInputName(idx_t) const override { return nullptr; }
    const char *getOutputName(idx_t) const override { return "out"; }

    // mutex() is protected on twComponent; the contention check below needs
    // to hold it from outside, which only a derived class can grant.
    std::mutex &testMutex() { return mutex(); }
};

int main()
{
    const offset_t CAP = (offset_t) twOutputPage::FRAME_CAPACITY;   // 65536

    // --- 1. The retention boundary, frame-exactly -----------------------
    //
    // A page at startPos covers [startPos, startPos + CAP). It is released
    // exactly when that half-open range ENDS BEFORE keepAfterPos, i.e. when
    //     startPos + CAP < keepAfterPos.
    // Both sides of that `<` are checked, one frame apart, on real pages.
    {
        tw303aEnvironment env;
        auto c = std::make_shared<BareComponent>(env);
        c->init();

        for (int i = 0; i < 4; ++i) {
            c->getOrAllocatePage((offset_t) i * CAP);
        }
        CHECK(c->pageStats().pages == 4, "four pages allocated");

        // keepAfterPos exactly at page 0's end: page 0 is NOT yet strictly
        // before it, so nothing goes. This is the frame the old byte-based
        // expression could never have discriminated.
        c->releaseOldPages(CAP);
        CHECK(c->pageStats().pages == 4,
              "keepAfterPos == page 0 end: page 0 is retained (boundary, closed side)");

        // One frame past it: page 0 goes and only page 0.
        c->releaseOldPages(CAP + 1);
        CHECK(c->pageStats().pages == 3,
              "keepAfterPos == page 0 end + 1: page 0 is released (boundary, open side)");
        CHECK(c->getPageIfExists(0) == nullptr, "…and it is page 0 that went");
        CHECK(c->getPageIfExists(CAP) != nullptr, "…while page 1 stayed");

        // The old, byte-based expression kept a page until keepAfterPos passed
        // startPos + 262144, i.e. four pages' worth. At this position pages 0
        // AND 1 must both be gone; under the bug page 1 (and 0) would survive.
        c->releaseOldPages(3 * CAP);
        CHECK(c->pageStats().pages == 2,
              "retention window is one page wide, not four (the frames-vs-bytes bug)");
        CHECK(c->getPageIfExists(CAP) == nullptr, "page 1 released at 3*CAP");
        CHECK(c->getPageIfExists(2 * CAP) != nullptr, "page 2 retained at 3*CAP");

        // Everything before the very end goes; the last page never does.
        c->releaseOldPages(100 * CAP);
        CHECK(c->pageStats().pages == 0, "a far-future keepAfterPos empties the cache");
    }

    // --- 2. Global accounting rides the page's own lifetime -------------
    {
        const uint64_t basePages = tw::pages::PageAccounting::global().pages;
        const uint64_t baseBytes = tw::pages::PageAccounting::global().bytes;
        const size_t   PAGE_BYTES = twOutputPage::FRAME_CAPACITY * sizeof(float);

        {
            auto p = std::make_shared<twOutputPage>();
            CHECK(tw::pages::PageAccounting::global().pages == basePages + 1,
                  "a page constructed anywhere is counted");
            CHECK(tw::pages::PageAccounting::global().bytes == baseBytes + PAGE_BYTES,
                  "…for its sample bytes (65536 frames x 4 = 262144)");
            CHECK(p->accountedBytes() == PAGE_BYTES,
                  "the page reports the byte count it registered");
        }
        CHECK(tw::pages::PageAccounting::global().pages == basePages,
              "and uncounted when the last reference goes");
        CHECK(tw::pages::PageAccounting::global().bytes == baseBytes,
              "…bytes too, exactly");

        // The counter does NOT depend on a component owning the page: a page
        // bound into a scheduler node or held by an audio callback is resident
        // memory, and a pool-side counter would have missed exactly those.
        const uint64_t everBefore = tw::pages::PageAccounting::everAllocated();
        { auto a = std::make_shared<twOutputPage>();
          auto b = std::make_shared<twOutputPage>(); }
        CHECK(tw::pages::PageAccounting::everAllocated() == everBefore + 2,
              "the cumulative counter counts every page ever built");
        CHECK(tw::pages::PageAccounting::peakBytes() >= baseBytes + 2 * PAGE_BYTES,
              "the high-water mark saw both of them at once");
    }

    // --- 3. Per-component accounting ------------------------------------
    {
        tw303aEnvironment env;
        const size_t PAGE_BYTES = twOutputPage::FRAME_CAPACITY * sizeof(float);

        const size_t beforePages = twComponent::componentPageStats().pages;

        auto a = std::make_shared<BareComponent>(env);
        a->init();
        auto b = std::make_shared<BareComponent>(env);
        b->init();

        for (int i = 0; i < 3; ++i) a->getOrAllocatePage((offset_t) i * CAP);
        for (int i = 0; i < 5; ++i) b->getOrAllocatePage((offset_t) i * CAP);

        CHECK(a->pageStats().pages == 3 && b->pageStats().pages == 5,
              "each component reports its own resident pages");
        CHECK(a->pageStats().bytes == 3 * PAGE_BYTES,
              "…and their bytes");
        CHECK(a->pageStats().frozen == 0,
              "a merely allocated page is resident but not frozen");

        CHECK(twComponent::componentPageStats().pages == beforePages + 8,
              "the per-component total sums over the live registry");

        // The description is what a test hook prints; it must at least name the
        // component type and carry both totals, or it is not a diagnosis.
        const std::string text = twComponent::describePageMemory("graph_test");
        CHECK(text.find("inComponents=") != std::string::npos,
              "describePageMemory reports the in-component total");
        CHECK(text.find("global=") != std::string::npos,
              "…and the global total beside it");
        CHECK(text.find("BareComponent") != std::string::npos,
              "…and names the component type holding the pages");

        // The try-lock variant ADDS into its argument and reports whether it got
        // the lock. Both halves matter: the registry walk relies on the return
        // value to count what it skipped, and on the accumulation to sum without
        // a temporary per component.
        {
            twComponent::PageStats acc;
            const bool gotA = a->pageStatsTry(acc);
            const bool gotB = b->pageStatsTry(acc);
            CHECK(gotA && gotB, "pageStatsTry acquires an uncontended component mutex");
            CHECK(acc.pages == 8, "…and ACCUMULATES rather than overwriting");

            std::unique_lock<std::mutex> held(a->testMutex());
            twComponent::PageStats busy;
            CHECK(!a->pageStatsTry(busy),
                  "…and refuses a held mutex instead of waiting for it "
                  "(the walk holds the registry lock; waiting here would invert)");
            CHECK(busy.pages == 0, "…leaving the accumulator untouched when it refuses");
        }

        // A destroyed component leaves the registry, and its pages leave the
        // global counter with it. This is the check that a leaked registry entry
        // would fail — and a leak there is a dangling pointer, not a statistic.
        const uint64_t globalWithB = tw::pages::PageAccounting::global().pages;
        b.reset();
        CHECK(twComponent::componentPageStats().pages == beforePages + 3,
              "a destroyed component is out of the per-component total");
        CHECK(tw::pages::PageAccounting::global().pages == globalWithB - 5,
              "…and its pages are out of the global one");
    }

    if (failures == 0) { printf("all graph tests passed\n"); return 0; }
    printf("%d graph test(s) FAILED\n", failures);
    return 1;
}
