// tw/mix module test: the ClipEntry model of twTrackMix against a scripted
// stub component — clip-relative positions, MapPosFn translation, the
// clip-end clamp, key-based update/remove. Normative background:
// docs/contracts/CLIP_MODEL.md and POSITION_DOMAINS.md.
#include "tw/mix/twtrackmix.h"
#include "tw/mix/twrewire.h"
#include "tw/mix/twgainstage.h"
#include "tw/graph/twcomponent.h"
#include "tw/graph/tw303aenv.h"
#include "tw/graph/tw_frozen_inputs.h"
#include "tw/pages/io_vector.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <memory>
#include <thread>

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
    int renders = 0;   // how many times material was actually produced

    bool isSeekable() const override { return true; }
    int seekTo(offset_t p) override { pos = p; return 0; }
    void reset() override { pos = 0; }
    length_t renderFrames(sample_t *out, length_t n, const sample_t *,
                          length_t, idx_t) override {
        ++renders;
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

// Like twSampleReader, a ramp whose single pos cursor makes its freezes
// serialize. The concurrency regression below needs every component EXCEPT
// the latch consumer under test to be race-free by construction.
class SerialRampComponent : public RampComponent {
public:
    using RampComponent::RampComponent;
    bool usesSerialCursor() const override { return true; }
};

// A WIDE source for the proposal 36 B4 tests: channel c carries val(pos) scaled
// by (c + 1), so a channel that ended up holding another channel's audio — or
// another page's — names itself.
class WideRampComponent : public twComponent {
public:
    WideRampComponent(tw303aEnvironment &e, idx_t ch)
        : twComponent(e), ch_(ch < 1 ? 1 : ch) {}
    offset_t pos = 0;

    static float value(idx_t c, long long p) { return val(p) * (float)(c + 1); }

    idx_t getOutputChannels() const override { return ch_; }
    bool isSeekable() const override { return true; }
    int seekTo(offset_t p) override { pos = p; return 0; }
    void reset() override { pos = 0; }
    bool usesSerialCursor() const override { return true; }

    length_t renderPageWide(twOutputPage &page, length_t frames,
                            const sample_t *, length_t) override {
        length_t n = frames;
        if (n > (length_t)page.channelFrames()) n = (length_t)page.channelFrames();
        for (idx_t c = 0; c < (idx_t)page.channels(); ++c) {
            sample_t *dst = page.channelPtr(c);
            for (length_t i = 0; i < n; ++i)
                dst[i] = value(c, (long long)(pos + i));
        }
        pos += (offset_t)n;   // ONCE, not per channel (proposal 36 4.3)
        return n;
    }
    // The narrow degradation (proposal 36 trap 18).
    length_t renderFrames(sample_t *out, length_t n, const sample_t *,
                          length_t, idx_t) override {
        for (length_t i = 0; i < n; ++i) out[i] = value(0, (long long)(pos + i));
        pos += (offset_t)n;
        return n;
    }
    void createOutputLatches() override {
        pOutputLatches_.resize(1);
        pOutputLatches_[0] =
            std::make_shared<twStreamingLatch>(shared_from_this(), 0, 0);
    }
    idx_t getNInputs() const override { return 0; }
    idx_t getNOutputs() const override { return 1; }
    const char *getInputName(idx_t) const override { return nullptr; }
    const char *getOutputName(idx_t) const override { return "wideramp"; }
private:
    idx_t ch_;
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

    const float *s = page->channelPtr(0);
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
    CHECK(page2->channelPtr(0)[(size_t)clipStart + 499] != 0.0f
              && page2->channelPtr(0)[(size_t)clipStart + 500] == 0.0f,
          "updateClip(key) changes the audible window");

    // Key-based removal: the WRONG key must remove nothing.
    const int wrongKey = 0;
    track->removeClip(&wrongKey);
    auto page3 = track->freezePage(0, nullptr, 0, PAGE, env.getSRate(), nullptr);
    CHECK(page3->channelPtr(0)[(size_t)clipStart] != 0.0f,
          "removeClip with a different key leaves the clip alone");

    track->removeClip(&dummyKeyA);
    auto page4 = track->freezePage(0, nullptr, 0, PAGE, env.getSRate(), nullptr);
    CHECK(page4->channelPtr(0)[(size_t)clipStart] == 0.0f,
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
        CHECK(a1->channelPtr(0)[(size_t)clipStart] != 0.0f,
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
        CHECK(a2->channelPtr(0)[(size_t)clipStart + 499] != 0.0f
                  && a2->channelPtr(0)[(size_t)clipStart + 500] == 0.0f,
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
        CHECK(q0->channelPtr(0)[(size_t)cStart] != 0.0f && q2->channelPtr(0)[1000] != 0.0f,
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
        CHECK(q2b->channelPtr(0)[1000 + 499] != 0.0f && q2b->channelPtr(0)[1000 + 500] == 0.0f,
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
        CHECK(q0c->channelPtr(0)[(size_t)cStart + 499] != 0.0f
                  && q0c->channelPtr(0)[(size_t)cStart + 500] == 0.0f,
              "page 0's re-render reflects clip C's edit");
    }

    // ------------------------------------------------------------------
    // Proposal 19 dataflow stage 1: the LEAF RENDERER renders from BOUND
    // input pages with no recursive pull (twFrozenInputs served at the
    // twStreamingLatch::copyData seam). A rewire over one track:
    //   1) classic pull-freeze = baseline samples,
    //   2) stale the track (epoch bump), then freeze the rewire AGAIN with
    //      the track's OLD page BOUND — must serve the bound page (baseline
    //      content, no source re-render: the pull is bypassed),
    //   3) control: same stale state, EMPTY set — legacy pull re-renders
    //      the source (proves 2 really bypassed it).
    {
        auto trackS = std::make_shared<twTrackMix>(env);
        trackS->init();
        auto compS = std::make_shared<RampComponent>(env);
        compS->init();
        const int keyS = 0;
        trackS->insertClip(&keyS, 1000, 3000,
                           [compS]() -> std::shared_ptr<twComponent> { return compS; });

        auto rewS = std::make_shared<twRewire>(env);
        rewS->init();
        rewS->setNPlugs(1);
        rewS->setInput(0, trackS->linkOutput(0));

        const length_t FULL = (length_t)twOutputPage::FRAME_CAPACITY;

        // 1) Baseline: classic pull path.
        auto base = rewS->freezePage(0, nullptr, 0, FULL, env.getSRate(), nullptr);
        CHECK(base && base->validFrames == FULL && base->channelPtr(0)[1000] != 0.0f,
              "leaf-renderer test: pull baseline freezes");

        // Take the TRACK page to bind (twTrackMix mints a page per call).
        auto trackPage = trackS->freezePage(0, nullptr, 0, FULL, env.getSRate(), nullptr);
        CHECK(trackPage && trackPage->validAspects != 0,
              "leaf-renderer test: track page frozen for binding");

        // 2) Stale the WHOLE upstream chain (track AND source), then render
        //    the rewire from the BOUND page. Staling the source matters for
        //    the control in 3): with only the track staled, the legacy pull
        //    would serve the source's still-current page cache and the
        //    "did the pull run" signal (renderFrames count) would not fire.
        trackS->bumpContentEpoch();
        compS->bumpContentEpoch();
        int rendersBefore = compS->renders;

        twFrozenInputs inputs;
        inputs.bind(trackS.get(), 0, trackPage);

        auto boundPage = std::make_shared<twOutputPage>();
        boundPage->startPosition = 0;
        length_t n = rewS->freezePageFromInputs(boundPage, inputs, nullptr);

        CHECK(n == FULL, "leaf renderer produces a full page");
        CHECK(compS->renders == rendersBefore,
              "bound input page served WITHOUT re-rendering the source");
        CHECK(inputs.misses.empty(),
              "no misses recorded when the input set covers the render");
        bool same = true;
        for (size_t i = 0; i < (size_t)FULL; ++i)
            if (boundPage->channelPtr(0)[i] != base->channelPtr(0)[i]) { same = false; break; }
        CHECK(same, "bound-input render is byte-identical to the pull baseline");

        // 3) Control: EMPTY set falls back to the legacy pull, which must
        //    re-render the (staled) track from the source.
        twFrozenInputs empty;
        auto ctrlPage = std::make_shared<twOutputPage>();
        ctrlPage->startPosition = 0;
        rewS->freezePageFromInputs(ctrlPage, empty, nullptr);
        CHECK(compS->renders > rendersBefore,
              "empty set falls back to the legacy pull (source re-renders)");
        CHECK(!empty.misses.empty(),
              "the fallback records the missing dependency for the scheduler");
    }

    // ------------------------------------------------------------------
    // Proposal 19 dataflow stage 2: the PLANNER. planPage() captures a
    // node's structural snapshot — which producer pages its render will
    // consume — without rendering:
    //   A) a latch consumer (rewire) plans one grid-aligned dep per input;
    //   B) the trackmix plans, per overlapping clip, the resolveClip()-
    //      resolved {component, mappedPos} — matching what the render
    //      actually requests;
    //   C) end-to-end: freeze the planned deps, bind them, execute the
    //      TRACKMIX node via freezePageWithInputs — the direct child-freeze
    //      seam serves the bound page (no source re-render), byte-identical.
    {
        auto trackP = std::make_shared<twTrackMix>(env);
        trackP->init();
        auto compP = std::make_shared<RampComponent>(env);
        compP->init();
        const int keyP = 0;
        const offset_t pStart = 1000;
        const length_t pDur = 3000;
        const offset_t pSlip = 7000;
        trackP->insertClip(&keyP, pStart, pDur,
                           [compP]() -> std::shared_ptr<twComponent> { return compP; },
                           [=](offset_t off) {
                               return twResolvedClip{ compP, (offset_t)(off + pSlip) };
                           });

        auto rewP = std::make_shared<twRewire>(env);
        rewP->init();
        rewP->setNPlugs(1);
        rewP->setInput(0, trackP->linkOutput(0));

        const length_t FULL = (length_t)twOutputPage::FRAME_CAPACITY;

        // A) Latch consumer: rewire's page-0 plan = one dep on (track, 0).
        twPagePlan rewPlan = rewP->planPage(0);
        CHECK(rewPlan.component.get() == rewP.get() && rewPlan.epoch > 0,
              "rewire plan carries the node identity and epoch");
        CHECK(rewPlan.deps.size() == 1 &&
              rewPlan.deps[0].producer.get() == trackP.get() &&
              rewPlan.deps[0].pageStart == 0,
              "latch consumer plans one grid-aligned dep per input");

        // B) Trackmix: page-0 plan = the clip's resolved {component, mappedPos}.
        //    Page 0 starts before the clip (childPos 0) -> mappedPos = slip.
        twPagePlan trackPlan = trackP->planPage(0);
        CHECK(trackPlan.deps.size() == 1 &&
              trackPlan.deps[0].producer.get() == compP.get() &&
              trackPlan.deps[0].pageStart == (uint64_t)pSlip,
              "trackmix plans the resolveClip()-resolved component + mappedPos");
        //    A page past the clip's end plans no deps.
        twPagePlan farPlan = trackP->planPage(4 * (uint64_t)FULL);
        CHECK(farPlan.deps.empty(), "a page with no overlapping clips plans empty");

        // C) End-to-end: baseline pull render, then plan-driven render.
        auto baseT = trackP->freezePage(0, nullptr, 0, FULL, env.getSRate(), nullptr);
        CHECK(baseT && baseT->channelPtr(0)[(size_t)pStart] == val(pSlip),
              "planner test: pull baseline renders the slipped clip");

        //    Freeze the planned dep exactly as planned, bind it.
        twFrozenInputs planInputs;
        for (const twPageDep &d : trackPlan.deps) {
            auto depPage = d.producer->freezePage(d.pageStart, nullptr, 0, FULL,
                                                  env.getSRate(), nullptr);
            planInputs.bind(d.producer.get(), d.pageStart, depPage);
        }

        //    Stale the source; the bound page must carry the content.
        compP->bumpContentEpoch();
        int rendersBefore = compP->renders;
        auto planPage0 = trackP->freezePageWithInputs(0, planInputs, nullptr);
        CHECK(planPage0 && planPage0->validFrames == FULL,
              "plan-driven trackmix node renders a full page");
        CHECK(compP->renders == rendersBefore,
              "direct child-freeze seam serves the bound page (no re-render)");
        CHECK(planInputs.misses.empty(),
              "a complete plan records no misses");
        bool sameT = true;
        for (size_t i = 0; i < (size_t)FULL; ++i)
            if (planPage0->channelPtr(0)[i] != baseT->channelPtr(0)[i]) { sameT = false; break; }
        CHECK(sameT, "plan-driven render is byte-identical to the pull baseline");
    }

    // ------------------------------------------------------------------
    // Regression (2026-07, test4_2.qxp "track three restarts its loop
    // mid-bar"): freezePage() of a latch-consuming component (root twMixer,
    // twRewire, …) was not serialized — usesSerialCursor() is false for
    // "pure" nodes — yet the input-side read position (twLatchOutput::offset)
    // is ONE shared field per plug, written by seekInputStreams() and
    // advanced by readStreamingData(). Two concurrent freezes of the SAME
    // consumer at DIFFERENT pages could interleave seek/read on that cursor,
    // so a page froze with its input stream read at the OTHER freeze's
    // position: a coherent page whose content is displaced by a whole page
    // multiple, then cached as current-epoch and replayed deterministically
    // by both playback and render. Trigger in the wild: overlapping
    // scheduler demands (readahead chain restart across a playhead jump, or
    // an edit invalidating pages behind an in-flight demand).
    //
    // The producers here are race-free by construction (trackmix serializes
    // under its own mutex, the ramp is a serial-cursor component), so any
    // displaced content must come from the rewire's shared input cursor.
    {
        auto trackC = std::make_shared<twTrackMix>(env);
        trackC->init();
        auto compC2 = std::make_shared<SerialRampComponent>(env);
        compC2->init();
        const int keyC2 = 0;
        const uint64_t CAP = twOutputPage::FRAME_CAPACITY;
        // One clip at 0 spanning four pages, identity mapping: the track
        // page at P carries val(P + i), so a displaced page is detectable.
        trackC->insertClip(&keyC2, 0, (length_t)(4 * CAP),
                           [compC2]() -> std::shared_ptr<twComponent> { return compC2; });

        auto rewC = std::make_shared<twRewire>(env);
        rewC->init();
        rewC->setNPlugs(1);
        rewC->setInput(0, trackC->linkOutput(0));

        const length_t FULL = (length_t)CAP;
        int displaced = 0;
        for (int round = 0; round < 200 && !displaced; ++round) {
            rewC->bumpContentEpoch();   // stale both pages: both freezes render
            std::atomic<bool> go{false};
            std::atomic<int> bad{0};
            auto freezeAt = [&](offset_t pos) {
                while (!go.load(std::memory_order_acquire)) { }
                auto p = rewC->freezePage(pos, nullptr, 0, FULL,
                                          env.getSRate(), nullptr);
                if (!p || p->validAspects == 0 || p->validFrames != FULL) {
                    ++bad;
                    return;
                }
                const size_t probes[] = { 0, 1234, (size_t)FULL - 1 };
                for (size_t k : probes) {
                    if (p->channelPtr(0)[k] != val((long long)pos + (long long)k)) {
                        ++bad;
                        break;
                    }
                }
            };
            std::thread t1(freezeAt, (offset_t)0);
            std::thread t2(freezeAt, (offset_t)CAP);
            go.store(true, std::memory_order_release);
            t1.join();
            t2.join();
            displaced = bad.load();
        }
        CHECK(displaced == 0,
              "concurrent freezes of one latch consumer never displace content");
    }

    // ------------------------------------------------------------------
    // A page holds exactly FRAME_CAPACITY frames, but inputLength is sized
    // from CONTENT by some callers (SCut::buildCapture_ hands over the whole
    // remaining capture length, which is millions of frames for a long
    // container). freezePage must clamp it: the fill of the page buffer and
    // the endPos that drives the clip-overlap walk both take it at face value,
    // so an over-long length is a heap overrun plus every clip in the track
    // dragged into this one page's mix.
    {
        auto trackL = std::make_shared<twTrackMix>(env);
        trackL->init();
        auto compL = std::make_shared<RampComponent>(env);
        compL->init();
        const int keyL = 0;
        const uint64_t CAP = twOutputPage::FRAME_CAPACITY;
        const length_t FULL = (length_t)CAP;

        // One clip at 0 spanning four pages, identity mapping.
        trackL->insertClip(&keyL, 0, (length_t)(4 * CAP),
                           [compL]() -> std::shared_ptr<twComponent> { return compL; });

        auto ref = trackL->freezePage(0, nullptr, 0, FULL, env.getSRate(), nullptr);
        CHECK(ref && ref->validFrames == FULL,
              "page-length clamp: full-page reference freeze");

        // 64 pages' worth of frames in one request.
        auto huge = trackL->freezePage(0, nullptr, 0, (length_t)(64 * CAP),
                                       env.getSRate(), nullptr);
        CHECK(huge && huge->validFrames == FULL,
              "an over-long inputLength is clamped to one page of valid frames");
        CHECK(huge && huge->channelFrames() == (size_t)CAP,
              "the page buffer is not grown or overrun by an over-long length");
        bool sameL = huge && ref;
        for (size_t i = 0; sameL && i < (size_t)FULL; ++i)
            if (huge->channelPtr(0)[i] != ref->channelPtr(0)[i]) sameL = false;
        CHECK(sameL, "the clamped render is identical to the full-page render");

        // inputLength == 0 means "no input data supplied", not "render
        // nothing" — RenderSession pulls the graph root with 0 and expects a
        // full page (render_session.cc).
        auto zero = trackL->freezePage(0, nullptr, 0, 0, env.getSRate(), nullptr);
        CHECK(zero && zero->validFrames == FULL,
              "inputLength == 0 renders a full page, not an empty one");
    }

    // ------------------------------------------------------------------
    // PROPOSAL 36 B4: twRewire is a CHANNEL-MAPPING component.
    //
    // sstdmixer.cpp carried "FIXME: Generate a channel reassignment." over
    // getRootComponent() from the beginning. B4 discharges it here: a wide
    // rewire reads its input PAGE (4.4 rule 2) and publishes output channel c
    // from that page's channel map[c] — identity by default, and CLAMPED
    // against the page in hand. Nothing in production sets a non-identity map
    // yet, so this is the only thing that makes the claim true rather than
    // asserted in a comment.
    {
        const uint64_t CAP = twOutputPage::FRAME_CAPACITY;
        const idx_t W = 4;
        auto src = std::make_shared<WideRampComponent>(env, W);
        src->init();
        auto rew = std::make_shared<twRewire>(env);
        rew->init();
        rew->setNPlugs(1);                 // a wide rewire is single-plug
        rew->setChannels(W);
        rew->setInput(0, src->linkOutput(0));

        auto page = rew->freezePage(0, nullptr, 0, (length_t)CAP,
                                    env.getSRate(), nullptr);
        CHECK(page && page->channels() == W,
              "rewire: the page is the declared width");
        bool identity = page != nullptr;
        for (idx_t c = 0; identity && c < W; ++c)
            for (long long i = 0; identity && i < 512; ++i)
                identity = page->channelPtr(c)[i] == WideRampComponent::value(c, i);
        CHECK(identity, "rewire: the default map is the identity");

        // Reverse it. The map is OUTPUT-indexed, so out[0] must become in[3] —
        // and the epoch bump inside setChannelMap is what makes the change
        // visible instead of serving the page that is already cached.
        rew->setChannelMap({3, 2, 1, 0});
        auto rev = rew->freezePage(0, nullptr, 0, (length_t)CAP,
                                   env.getSRate(), nullptr);
        bool reversed = rev != nullptr && rev != page;
        for (idx_t c = 0; reversed && c < W; ++c)
            for (long long i = 0; reversed && i < 512; ++i)
                reversed = rev->channelPtr(c)[i] ==
                           WideRampComponent::value((idx_t)(W - 1 - c), i);
        CHECK(reversed, "rewire: setChannelMap reassigns channels, and stales the "
                        "pages rendered under the old map");

        // A map naming a channel the PRODUCER does not have degrades to that
        // producer's LAST channel (4.4's clamp) instead of reading out of
        // bounds. A mono producer under the map {3,3,3,3} is the extreme case.
        auto mono = std::make_shared<WideRampComponent>(env, 1);
        mono->init();
        auto rew2 = std::make_shared<twRewire>(env);
        rew2->init();
        rew2->setNPlugs(1);
        rew2->setChannels(W);
        rew2->setChannelMap({3, 3, 3, 3});
        rew2->setInput(0, mono->linkOutput(0));
        auto clamped = rew2->freezePage(0, nullptr, 0, (length_t)CAP,
                                        env.getSRate(), nullptr);
        bool allCh0 = clamped != nullptr && clamped->channels() == W;
        for (idx_t c = 0; allCh0 && c < W; ++c)
            for (long long i = 0; allCh0 && i < 512; ++i)
                allCh0 = clamped->channelPtr(c)[i] == WideRampComponent::value(0, i);
        CHECK(allCh0, "rewire: a map naming a channel the producer lacks clamps to "
                      "its last one, never reads out of bounds");
    }

    // ---- twGainStage: THE FADER, post-FX (proposal 37 P3a / D5) -----------
    //
    // It is one wide component per track between the plugin chain and the
    // rewire. What is pinned here is what the qxa cases cannot see: that unity
    // is BIT-EXACT (the byte-identity argument for proposal 36's committed
    // golden corpus rests on it), that a gain change stales the pages already
    // rendered, and that the mute ramp is POSITION-DETERMINISTIC -- which is
    // what makes the component class infinity and its range invalidation exact.
    {
        const idx_t W = 4;
        const length_t CAP = (length_t)twOutputPage::FRAME_CAPACITY;

        auto src = std::make_shared<WideRampComponent>(env, W);
        src->init();
        auto gs = std::make_shared<twGainStage>(env);
        gs->init();
        gs->setChannels(W);
        gs->setInput(0, src->linkOutput(0));

        auto unity = gs->freezePage(0, nullptr, 0, CAP, env.getSRate(), nullptr);
        bool exact = unity != nullptr && unity->channels() == W;
        for (idx_t c = 0; exact && c < W; ++c)
            for (long long i = 0; exact && i < 4096; ++i)
                exact = unity->channelPtr(c)[i] == WideRampComponent::value(c, i);
        CHECK(exact, "gainstage: 0 dB is a BIT-EXACT pass-through of every channel");

        // -6.0206 dB is 0.4999995..., the fader spelling of a half. Asserted as
        // the exact float product, not as "about half": the render is ONE
        // multiply, so anything else means a second operation crept in.
        gs->setGainDb(-6.0206);
        auto half = gs->freezePage(0, nullptr, 0, CAP, env.getSRate(), nullptr);
        const sample_t g = (sample_t)std::pow(10., -6.0206 / 20.);
        bool halved = half != nullptr && half->channels() == W;
        for (idx_t c = 0; halved && c < W; ++c)
            for (long long i = 0; halved && i < 4096; ++i)
                halved = half->channelPtr(c)[i] == WideRampComponent::value(c, i) * g;
        CHECK(halved, "gainstage: setGainDb scales every channel, and stales the "
                      "page rendered at the old gain");

        // Mute with no anchor is flat silence, everywhere.
        gs->setGainDb(0.0);
        gs->setMuted(true);
        auto quiet = gs->freezePage(0, nullptr, 0, CAP, env.getSRate(), nullptr);
        bool silent = quiet != nullptr;
        for (idx_t c = 0; silent && c < W; ++c)
            for (long long i = 0; silent && i < 4096; ++i)
                silent = quiet->channelPtr(c)[i] == 0.0f;
        CHECK(silent, "gainstage: an unanchored mute is silence on every channel");

        // THE RAMP, and the reason it takes a POSITION rather than running a
        // state machine: the anchor is placed in the SECOND page, and the second
        // page is rendered BEFORE the first. A ramp that counted frames as it saw
        // them would put the fade in the wrong place; a positional one cannot.
        const offset_t anchor = (offset_t)CAP + 1000;
        const length_t ramp = gs->muteRampFrames();
        gs->setMuted(false);                       // clear, then re-arm anchored
        gs->setMuted(true, anchor);

        auto p1 = gs->freezePage((offset_t)CAP, nullptr, 0, CAP, env.getSRate(), nullptr);
        auto p0 = gs->freezePage(0, nullptr, 0, CAP, env.getSRate(), nullptr);

        bool beforeIsUntouched = p0 != nullptr;
        for (long long i = 0; beforeIsUntouched && i < 4096; ++i)
            beforeIsUntouched = p0->channelPtr(0)[i] == WideRampComponent::value(0, i);
        CHECK(beforeIsUntouched, "gainstage: a page entirely before the mute anchor "
                                 "is untouched, even when rendered after the one "
                                 "holding the ramp");

        // Flat 1.0 up to the anchor, flat 0.0 from anchor + ramp. Index i of
        // page 1 is absolute frame CAP + i.
        bool rampOk = p1 != nullptr && ramp > 1;
        for (long long i = 0; rampOk && i < 1000; ++i)
            rampOk = p1->channelPtr(0)[i] ==
                     WideRampComponent::value(0, (long long)CAP + i);
        for (long long i = 1000 + (long long)ramp;
             rampOk && i < 1000 + (long long)ramp + 512; ++i)
            rampOk = p1->channelPtr(0)[i] == 0.0f;
        CHECK(rampOk, "gainstage: the mute ramp starts exactly at its anchor and is "
                      "complete after muteRampFrames()");

        bool monotone = p1 != nullptr;
        double prev = 2.0;
        for (long long i = 1000; monotone && i < 1000 + (long long)ramp; ++i) {
            const double ref = WideRampComponent::value(0, (long long)CAP + i);
            if (ref == 0.0) continue;              // ratio undefined, skip
            const double f = p1->channelPtr(0)[i] / ref;
            monotone = f <= prev + 1e-6 && f >= -1e-6 && f <= 1.0 + 1e-6;
            prev = f;
        }
        CHECK(monotone, "gainstage: the mute ramp is monotone 1 -> 0 across its span");

        // The NARROW path (width 1) is the legacy pull: the base renderFrames()
        // forwards to calcOutputTo(), which reads the producer's plug and scales.
        // That is the path assert-meter and every pre-scheduler pull take.
        auto mono = std::make_shared<WideRampComponent>(env, 1);
        mono->init();
        auto gsm = std::make_shared<twGainStage>(env);
        gsm->init();
        gsm->setChannels(1);
        gsm->setInput(0, mono->linkOutput(0));
        gsm->setGainDb(-6.0206);
        auto narrow = gsm->freezePage(0, nullptr, 0, CAP, env.getSRate(), nullptr);
        bool narrowOk = narrow != nullptr && narrow->channels() == 1;
        for (long long i = 0; narrowOk && i < 4096; ++i)
            narrowOk = narrow->channelPtr(0)[i] == WideRampComponent::value(0, i) * g;
        CHECK(narrowOk, "gainstage: the width-1 (legacy pull) path applies the same "
                        "gain as the wide one");
    }

    printf(failures ? "\n%d FAILURE(S)\n" : "\nall mix tests passed\n",
           failures);
    return failures ? 1 : 0;
}
