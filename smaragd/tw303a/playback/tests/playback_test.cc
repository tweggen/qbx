// tw/playback module test: AudioEngine page adoption under mid-playback edits
// (proposal 16). An edit bumps the producer's content epoch; the audio thread
// must keep serving the stale-but-consistent pre-edit pages as a fallback —
// never silence — until the readahead re-freezes them, and must switch to the
// new content as soon as it lands. Also covers the component-level mechanism:
// a placeholder replacing a stale-frozen page keeps it reachable via
// stalePredecessor until the replacement is stamped frozen.
#include "tw/playback/audio_engine.h"
#include "tw/graph/twcomponent.h"
#include "tw/graph/tw303aenv.h"
#include "tw/pages/tw_output_page.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (cond) { printf("ok   %s\n", msg); }                             \
        else      { printf("FAIL %s\n", msg); ++failures; }                 \
    } while (0)

// Constant-amplitude source whose renders can be made artificially slow, so a
// test can observe the window while a stale page's replacement is rendering.
class ToneComponent : public twComponent {
public:
    explicit ToneComponent(tw303aEnvironment &e) : twComponent(e) {}
    std::atomic<float> amp{0.25f};
    std::atomic<int> renderDelayMs{0};
    offset_t pos = 0;

    bool isSeekable() const override { return true; }
    int seekTo(offset_t p) override { pos = p; return 0; }
    void reset() override { pos = 0; }
    length_t renderFrames(sample_t *out, length_t n, const sample_t *,
                          length_t, idx_t) override {
        const int delay = renderDelayMs.load();
        if (delay > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        const float a = amp.load();
        for (length_t i = 0; i < n; ++i) out[i] = a;
        pos += (offset_t)n;
        return n;
    }
    void createOutputLatches() override {}
    idx_t getNInputs() const override { return 0; }
    idx_t getNOutputs() const override { return 1; }
    const char *getInputName(idx_t) const override { return nullptr; }
    const char *getOutputName(idx_t) const override { return "tone"; }
};

int main()
{
    tw303aEnvironment env;

    // ------------------------------------------------------------------
    // Engine-level: edit mid-playback must degrade to STALE audio, not
    // silence, and the new content must become audible without a dropout.
    {
        // twComponent uses shared_from_this() (createOutputLatches, freezePage),
        // and AudioEngine now takes a std::shared_ptr<twComponent>, so the
        // source must be heap-owned rather than a stack local.
        auto src = std::make_shared<ToneComponent>(env);
        src->init();

        audio::AudioEngine engine(src, (uint32_t)env.getSRate());
        engine.startReadahead();

        constexpr length_t BLOCK = 512;
        std::vector<float> L(BLOCK), R(BLOCK);

        // Wait for the readahead to buffer; then audio flows
        bool audible = false;
        for (int i = 0; i < 500 && !audible; ++i) {
            length_t n = engine.pullBlock(L.data(), R.data(), BLOCK);
            if (n == BLOCK && L[0] == 0.25f) { audible = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(audible, "playback starts and carries the original content");

        // The edit: new amplitude, and make the re-freeze slow enough that
        // the next pullBlock is guaranteed to land inside the re-render
        // window (readahead wakes within 20ms, then sleeps 300ms rendering).
        src->amp.store(0.5f);
        src->renderDelayMs.store(300);
        src->bumpContentEpoch();

        length_t n = engine.pullBlock(L.data(), R.data(), BLOCK);
        CHECK(n == BLOCK,
              "pullBlock immediately after an edit still produces frames");
        CHECK(n == BLOCK && L[0] == 0.25f && L[BLOCK - 1] == 0.25f,
              "fallback frames carry the consistent PRE-edit content");

        // Keep pulling at roughly realtime pace: the post-edit content must
        // arrive, and no pull in between may come up short (silence).
        src->renderDelayMs.store(0);
        bool freshHeard = false, dropout = false;
        for (int i = 0; i < 500 && !freshHeard && !dropout; ++i) {
            length_t got = engine.pullBlock(L.data(), R.data(), BLOCK);
            if (got != BLOCK) { dropout = true; break; }
            if (L[0] == 0.5f) { freshHeard = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(!dropout, "no dropout between the edit and the re-freeze");
        CHECK(freshHeard, "post-edit content becomes audible after re-freeze");

        engine.stopReadahead();
    }

    // ------------------------------------------------------------------
    // Component-level: replacing a stale-frozen page keeps the pre-edit
    // page reachable (stalePredecessor) until the replacement is frozen.
    {
        auto src = std::make_shared<ToneComponent>(env);
        src->init();

        const length_t FULL = (length_t)twOutputPage::FRAME_CAPACITY;
        auto oldPage = src->freezePage(0, nullptr, 0, FULL, env.getSRate(),
                                       nullptr);
        CHECK(oldPage && oldPage->validAspects != 0 &&
                  oldPage->samples[0] == 0.25f,
              "initial page freezes with the original content");

        src->amp.store(0.75f);
        src->renderDelayMs.store(300);
        src->bumpContentEpoch();

        std::shared_ptr<twOutputPage> freshPage;
        std::thread rerender([&] {
            freshPage = src->freezePage(0, nullptr, 0, FULL, env.getSRate(),
                                        nullptr);
        });

        // The re-freeze replaces the map entry with a placeholder right away,
        // then spends 300ms rendering; observe the entry inside that window.
        std::shared_ptr<twOutputPage> placeholder;
        for (int i = 0; i < 200; ++i) {
            placeholder = src->getPageIfExists(0);
            if (placeholder && placeholder.get() != oldPage.get()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(placeholder && placeholder.get() != oldPage.get(),
              "re-freeze replaces the stale map entry with a placeholder");
        CHECK(placeholder && placeholder->validAspects == 0,
              "placeholder is not yet frozen while its render runs");
        auto predecessor = placeholder
            ? std::atomic_load(&placeholder->stalePredecessor)
            : std::shared_ptr<twOutputPage>{};
        CHECK(predecessor.get() == oldPage.get(),
              "placeholder keeps the pre-edit page reachable while rendering");

        rerender.join();
        CHECK(freshPage && freshPage->validAspects != 0 &&
                  freshPage->samples[0] == 0.75f,
              "re-frozen page carries the post-edit content");
        CHECK(freshPage &&
                  std::atomic_load(&freshPage->stalePredecessor) == nullptr,
              "pre-edit page is released once the replacement is frozen");
    }

    printf(failures ? "\n%d FAILURE(S)\n" : "\nall playback tests passed\n",
           failures);
    return failures ? 1 : 0;
}
