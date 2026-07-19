// tw/mix module test: the ClipEntry model of twTrackMix against a scripted
// stub component — clip-relative positions, MapPosFn translation, the
// clip-end clamp, key-based update/remove. Normative background:
// docs/contracts/CLIP_MODEL.md and POSITION_DOMAINS.md.
#include "tw/mix/twtrackmix.h"
#include "tw/mix/twrewire.h"
#include "tw/graph/twcomponent.h"
#include "tw/graph/tw303aenv.h"
#include "tw/pages/io_vector.h"

#include <cstdio>
#include <memory>

static int failures = 0;
#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (cond) { printf("ok   %s\n", msg); }                             \
        else      { printf("FAIL %s\n", msg); ++failures; }                 \
    } while (0)

// Never-zero, position-identifying value (so silence is distinguishable).
static float val(long long p) { return (float)((p % 977) + 1) / 1000.0f; }

// Scripted source: renderFrames() emits val(position) and advances; seekTo
// sets the position in the COMPONENT'S OWN domain (a MapPosFn adds the slip
// offset before this component ever sees a position).
class RampComponent : public twComponent {
public:
    explicit RampComponent(tw303aEnvironment &e) : twComponent(e) {}
    offset_t pos = 0;

    bool isSeekable() const override { return true; }
    int seekTo(offset_t p) override { pos = p; return 0; }
    void reset() override { pos = 0; }
    length_t renderFrames(sample_t *out, length_t n, const sample_t *,
                          length_t, idx_t) override {
        for (length_t i = 0; i < n; ++i) out[i] = val((long long)(pos + i));
        pos += (offset_t)n;
        return n;
    }
    void createOutputLatches() override {}
    idx_t getNInputs() const override { return 0; }
    idx_t getNOutputs() const override { return 1; }
    const char *getInputName(idx_t) const override { return nullptr; }
    const char *getOutputName(idx_t) const override { return "ramp"; }
};

int main()
{
    tw303aEnvironment env;

    // twComponent derives from std::enable_shared_from_this and calls
    // shared_from_this() during init() (createOutputLatches) and freezePage(),
    // so every component must be owned by a std::shared_ptr — it can no longer
    // live as a stack local. getComponentFn now hands back a shared_ptr too.
    auto track = std::make_shared<twTrackMix>(env);
    track->init();

    auto comp = std::make_shared<RampComponent>(env);
    comp->init();

    const int dummyKeyA = 0;                 // opaque identity — NOT the component
    const offset_t clipStart = 1000;
    const length_t clipDur = 3000;
    const offset_t slip = 7000;              // the clip's media offset

    // Inv-1: the freeze/seek path takes a single resolver returning
    // {component, mappedPos} atomically. Here the mapping just folds the slip.
    track->insertClip(&dummyKeyA, clipStart, clipDur,
                      [comp]() -> std::shared_ptr<twComponent> { return comp; },
                      [=](offset_t off) {
                          return twResolvedClip{ comp, (offset_t)(off + slip) };
                      });

    const length_t PAGE = 8192;
    auto page = track->freezePage(0, nullptr, 0, PAGE, env.getSRate(), nullptr);
    CHECK(page && page->validFrames == PAGE, "track page freezes");

    const auto &s = page->samples;
    CHECK(s[0] == 0.0f && s[(size_t)clipStart - 1] == 0.0f,
          "silence before the clip start");
    CHECK(s[(size_t)clipStart] == val(slip),
          "clip start plays the resolver-translated (slipped) material");
    CHECK(s[(size_t)clipStart + clipDur - 1] == val(slip + clipDur - 1),
          "material is continuous through the clip");
    CHECK(s[(size_t)(clipStart + clipDur)] == 0.0f
              && s[(size_t)PAGE - 1] == 0.0f,
          "mix is CLAMPED at the clip end (no page bleed)");

    // Key-based update: shrink the duration; a fresh page must honor it.
    track->updateClip(&dummyKeyA, clipStart, 500);
    auto page2 = track->freezePage(0, nullptr, 0, PAGE, env.getSRate(), nullptr);
    CHECK(page2->samples[(size_t)clipStart + 499] != 0.0f
              && page2->samples[(size_t)clipStart + 500] == 0.0f,
          "updateClip(key) changes the audible window");

    // Key-based removal: the WRONG key must remove nothing.
    const int wrongKey = 0;
    track->removeClip(&wrongKey);
    auto page3 = track->freezePage(0, nullptr, 0, PAGE, env.getSRate(), nullptr);
    CHECK(page3->samples[(size_t)clipStart] != 0.0f,
          "removeClip with a different key leaves the clip alone");

    track->removeClip(&dummyKeyA);
    auto page4 = track->freezePage(0, nullptr, 0, PAGE, env.getSRate(), nullptr);
    CHECK(page4->samples[(size_t)clipStart] == 0.0f,
          "removeClip with the right key silences the clip");

    // ------------------------------------------------------------------
    // Scoped invalidation (proposal 15): two independent track→rewire
    // chains share one environment. Editing track A and bumping A's path
    // must re-render A's rewire cache; B's cached page object must be
    // served untouched (the scoping property itself, not just audibility).
    {
        auto trackA = std::make_shared<twTrackMix>(env);
        auto trackB = std::make_shared<twTrackMix>(env);
        trackA->init();
        trackB->init();
        auto compA = std::make_shared<RampComponent>(env);
        auto compB = std::make_shared<RampComponent>(env);
        compA->init();
        compB->init();

        const int keyA = 0, keyB = 0;   // distinct addresses = distinct keys
        trackA->insertClip(&keyA, clipStart, clipDur,
                           [compA]() -> std::shared_ptr<twComponent> { return compA; });
        trackB->insertClip(&keyB, clipStart, clipDur,
                           [compB]() -> std::shared_ptr<twComponent> { return compB; });

        auto rewA = std::make_shared<twRewire>(env);
        auto rewB = std::make_shared<twRewire>(env);
        rewA->init();
        rewB->init();
        rewA->setNPlugs(1);
        rewB->setNPlugs(1);
        rewA->setInput(0, trackA->linkOutput(0));
        rewB->setInput(0, trackB->linkOutput(0));

        const length_t FULL = (length_t)twOutputPage::FRAME_CAPACITY;
        auto a1 = rewA->freezePage(0, nullptr, 0, FULL, env.getSRate(), nullptr);
        auto b1 = rewB->freezePage(0, nullptr, 0, FULL, env.getSRate(), nullptr);
        CHECK(a1 && a1->validAspects != 0 && b1 && b1->validAspects != 0,
              "both rewires freeze their first page");
        CHECK(a1->samples[(size_t)clipStart] != 0.0f,
              "rewire A's page carries track A's clip");

        auto a1again = rewA->freezePage(0, nullptr, 0, FULL, env.getSRate(), nullptr);
        CHECK(a1again.get() == a1.get(),
              "unedited re-freeze is a cache hit (same page object)");

        // Edit track A (engine mutation self-bumps the trackmix), then bump
        // A's downstream path the way SObject::invalidateRenderPath() does.
        trackA->updateClip(&keyA, clipStart, 500);
        rewA->bumpContentEpoch();

        auto a2 = rewA->freezePage(0, nullptr, 0, FULL, env.getSRate(), nullptr);
        CHECK(a2.get() != a1.get(),
              "edited path re-renders into a fresh page object");
        CHECK(a2->samples[(size_t)clipStart + 499] != 0.0f
                  && a2->samples[(size_t)clipStart + 500] == 0.0f,
              "re-rendered page reflects the edit (shrunk clip window)");

        auto b2 = rewB->freezePage(0, nullptr, 0, FULL, env.getSRate(), nullptr);
        CHECK(b2.get() == b1.get(),
              "SIBLING cache is untouched by the edit (scoped invalidation)");
    }

    // ------------------------------------------------------------------
    // RANGE-scoped invalidation (proposal 18 Phase 5): ONE track with two
    // clips in different pages, cached downstream by a rewire (twTrackMix
    // itself mints fresh pages; the page CACHES are the downstream
    // components — same layering the app's STrack::bumpRenderChainEpochRange
    // drives). Editing clip D re-renders only D's page range: the rewire
    // page over clip C is served as the SAME page object. And a page
    // already stale from an earlier edit must NOT be re-blessed by a
    // later, disjoint edit (that would resurrect outdated audio).
    {
        auto track2 = std::make_shared<twTrackMix>(env);
        track2->init();
        auto compC = std::make_shared<RampComponent>(env);
        auto compD = std::make_shared<RampComponent>(env);
        compC->init();
        compD->init();

        const uint64_t CAP = twOutputPage::FRAME_CAPACITY;
        const int keyC = 0, keyD = 0;            // distinct addresses
        const offset_t cStart = 1000;            // inside page 0
        const offset_t dStart = (offset_t)(2 * CAP + 1000);   // inside page 2
        track2->insertClip(&keyC, cStart, 3000,
                           [compC]() -> std::shared_ptr<twComponent> { return compC; });
        track2->insertClip(&keyD, dStart, 3000,
                           [compD]() -> std::shared_ptr<twComponent> { return compD; });

        auto rew = std::make_shared<twRewire>(env);
        rew->init();
        rew->setNPlugs(1);
        rew->setInput(0, track2->linkOutput(0));

        const length_t FULL = (length_t)CAP;
        auto q0 = rew->freezePage(0, nullptr, 0, FULL, env.getSRate(), nullptr);
        auto q2 = rew->freezePage(2 * CAP, nullptr, 0, FULL, env.getSRate(), nullptr);
        CHECK(q0 && q0->validAspects != 0 && q2 && q2->validAspects != 0,
              "both rewire pages of the two-clip track freeze");
        CHECK(q0->samples[(size_t)cStart] != 0.0f && q2->samples[1000] != 0.0f,
              "page 0 carries clip C, page 2 carries clip D");

        // Edit clip D only (shrink). The mutator reports the affected
        // extent — the union of the pre- and post-edit windows — and the
        // caller applies it downstream (as STrack::bumpRenderChainEpochRange
        // does for plugin chains and the rewire).
        twEditRange r = track2->updateClip(&keyD, dStart, 500);
        CHECK(r.start == (uint64_t)dStart && r.end == (uint64_t)dStart + 3000,
              "updateClip reports the union extent of the edit");
        rew->invalidatePagesInRange(r.start, r.end);

        auto q0b = rew->freezePage(0, nullptr, 0, FULL, env.getSRate(), nullptr);
        CHECK(q0b.get() == q0.get(),
              "page OUTSIDE the edit range is a cache hit (range scoping)");
        auto q2b = rew->freezePage(2 * CAP, nullptr, 0, FULL, env.getSRate(), nullptr);
        CHECK(q2b.get() != q2.get(),
              "page INSIDE the edit range re-renders");
        CHECK(q2b->samples[1000 + 499] != 0.0f && q2b->samples[1000 + 500] == 0.0f,
              "re-rendered page reflects the shrunk clip D");

        // Stale-page protection: stale page 0 via an edit at clip C, then
        // edit D again (disjoint). The disjoint edit re-blesses only pages
        // that were CURRENT — page 0 must stay stale and re-render with
        // clip C's edit, not serve pre-edit audio as current.
        twEditRange rc = track2->updateClip(&keyC, cStart, 500);
        rew->invalidatePagesInRange(rc.start, rc.end);   // page 0 goes stale
        twEditRange rd = track2->updateClip(&keyD, dStart, 400);
        rew->invalidatePagesInRange(rd.start, rd.end);   // disjoint edit
        auto q0c = rew->freezePage(0, nullptr, 0, FULL, env.getSRate(), nullptr);
        CHECK(q0c.get() != q0b.get(),
              "a stale page is NOT re-blessed by a disjoint later edit");
        CHECK(q0c->samples[(size_t)cStart + 499] != 0.0f
                  && q0c->samples[(size_t)cStart + 500] == 0.0f,
              "page 0's re-render reflects clip C's edit");
    }

    printf(failures ? "\n%d FAILURE(S)\n" : "\nall mix tests passed\n",
           failures);
    return failures ? 1 : 0;
}
