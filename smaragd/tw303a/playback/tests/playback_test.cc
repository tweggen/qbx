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
#include "tw/pages/capture_page_pool.h"
#include "tw/schedule/capture_revalidator.h"
#include "tw/core/position_code.h"

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

// Position-identifying, never-zero sample value (the mix_test pattern), so a
// pulled sample says WHICH absolute frame it came from and silence is
// distinguishable from content.
static float rampVal(long long p) { return (float)((p % 977) + 1) / 1000.0f; }

// A source whose every frame carries its own absolute position.
class RampComponent : public twComponent {
public:
    explicit RampComponent(tw303aEnvironment &e) : twComponent(e) {}
    offset_t pos = 0;

    bool isSeekable() const override { return true; }
    int seekTo(offset_t p) override { pos = p; return 0; }
    void reset() override { pos = 0; }
    length_t renderFrames(sample_t *out, length_t n, const sample_t *,
                          length_t, idx_t) override {
        for (length_t i = 0; i < n; ++i)
            out[i] = rampVal((long long)(pos + i));
        pos += (offset_t)n;
        return n;
    }
    void createOutputLatches() override {}
    idx_t getNInputs() const override { return 0; }
    idx_t getNOutputs() const override { return 1; }
    const char *getInputName(idx_t) const override { return nullptr; }
    const char *getOutputName(idx_t) const override { return "ramp"; }
};

// A source whose OUTPUT NAMES ITS OWN POSITION: it renders the integer-cycle
// tone staircase of tw/core/position_code.h as a function of the ABSOLUTE frame
// index it is being asked for, exactly as the on-disk fixture encodes source
// frames. Decode a block of what came out of the engine and you learn which
// position the engine actually pulled — not merely that it produced audio.
//
// This is the in-memory half of the harness. It shares the encoding with the
// fixture generator and the file decoder through position_code.h, so "the
// component agrees with the fixture" is true by construction rather than by
// two implementations happening to match.
class PositionCodedComponent : public twComponent {
public:
    explicit PositionCodedComponent(tw303aEnvironment &e) : twComponent(e) {}
    offset_t pos = 0;
    std::atomic<int> renderCalls{0};

    bool isSeekable() const override { return true; }
    int seekTo(offset_t p) override { pos = p; return 0; }
    void reset() override { pos = 0; }
    length_t renderFrames(sample_t *out, length_t n, const sample_t *,
                          length_t, idx_t) override {
        renderCalls.fetch_add(1);
        for (length_t i = 0; i < n; ++i) {
            out[i] = (sample_t)tw::poscode::sampleAtFrame((int64_t)pos + (int64_t)i);
        }
        pos += (offset_t)n;
        return n;
    }
    void createOutputLatches() override {}
    idx_t getNInputs() const override { return 0; }
    idx_t getNOutputs() const override { return 1; }
    const char *getInputName(idx_t) const override { return nullptr; }
    const char *getOutputName(idx_t) const override { return "coded"; }
};

// Decode a float buffer straight out of pullBlock(). Thin wrapper over the
// shared decoder — the widening to double is the only thing that belongs here.
static tw::poscode::Decode decodeFloats(const float *x, int64_t n)
{
    std::vector<double> d((size_t)n);
    for (int64_t i = 0; i < n; ++i) d[(size_t)i] = (double)x[(size_t)i];
    return tw::poscode::decodeBuffer(d.data(), n);
}


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
    // Live seek during playback (requestSeek): the RT pull thread adopts the
    // requested position without a restart/re-buffer, and playback resumes
    // from there. This is what click-to-seek routes through while playing.
    {
        auto src = std::make_shared<ToneComponent>(env);
        src->init();

        audio::AudioEngine engine(src, (uint32_t)env.getSRate());
        engine.startReadahead();

        constexpr length_t BLOCK = 512;
        std::vector<float> L(BLOCK), R(BLOCK);

        // Prime playback so the position has advanced away from 0.
        bool audible = false;
        for (int i = 0; i < 500 && !audible; ++i) {
            length_t n = engine.pullBlock(L.data(), R.data(), BLOCK);
            if (n == BLOCK && L[0] == 0.25f) audible = true;
            else std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        for (int i = 0; i < 8; ++i) engine.pullBlock(L.data(), R.data(), BLOCK);
        CHECK(audible && engine.currentPosition() > 0,
              "seek: playback running and advanced before the seek");

        // Forward live seek, page-aligned and far past the readahead frontier.
        const uint64_t TARGET = 4ull * (uint64_t)twOutputPage::FRAME_CAPACITY;
        engine.requestSeek(TARGET);
        // The very next pull adopts it (the RT pull is the sole writer of
        // currentPos_), even before any page at TARGET is frozen.
        engine.pullBlock(L.data(), R.data(), BLOCK);
        uint64_t after = engine.currentPosition();
        CHECK(after >= TARGET && after < TARGET + 4 * BLOCK,
              "seek: RT pull adopts the requested position");

        // Playback resumes from the new position (tone flows again).
        bool resumed = false;
        for (int i = 0; i < 500 && !resumed; ++i) {
            length_t n = engine.pullBlock(L.data(), R.data(), BLOCK);
            if (n == BLOCK && L[0] == 0.25f) resumed = true;
            else std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(resumed && engine.currentPosition() > TARGET,
              "seek: playback resumes and advances from the sought position");

        // A backward live seek repositions just the same.
        engine.requestSeek(0);
        engine.pullBlock(L.data(), R.data(), BLOCK);
        CHECK(engine.currentPosition() < TARGET,
              "seek: backward live seek repositions too");

        engine.stopReadahead();
    }

    // ------------------------------------------------------------------
    // Cycle wrap INSIDE one page: the audio must restart at loopStart.
    //
    // The wrap in pullBlock rewrites the playhead (pos = loopStart) and clears
    // prevFrozenPage_, but it does not touch the page read cursor. When the loop
    // region fits inside a single 65536-frame page, updateFrozenPage(loopStart)
    // finds the very page it is already holding — so if the cursor were merely
    // CARRIED rather than derived, the RT thread would keep reading at the
    // pre-wrap offset while the playhead reported loopStart: wrong-position
    // audio on every single wrap. Each frame here names its own position, so the
    // assertion is what the listener would hear, not a proxy for it.
    {
        auto src = std::make_shared<RampComponent>(env);
        src->init();

        audio::AudioEngine engine(src, (uint32_t)env.getSRate());

        constexpr length_t BLOCK = 4096;
        // Half a page: the wrap target lands inside the page already held.
        constexpr uint64_t LOOP_END = twOutputPage::FRAME_CAPACITY / 2;  // 32768
        static_assert(LOOP_END % BLOCK == 0,
                      "loop end must be block-aligned so the wrap falls on a "
                      "block boundary");
        constexpr int BLOCKS_TO_END = (int)(LOOP_END / BLOCK);

        engine.setLoopBoundaries(true, 0, LOOP_END);
        engine.startReadahead();

        std::vector<float> L(BLOCK), R(BLOCK);

        // Prime: wait for the readahead to freeze the first page.
        bool audible = false;
        for (int i = 0; i < 500 && !audible; ++i) {
            length_t n = engine.pullBlock(L.data(), R.data(), BLOCK);
            if (n == BLOCK && L[0] != 0.0f) audible = true;
            else std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(audible, "cycle: playback starts inside the loop region");

        // Put the playhead back on the loop start so the wrap is a known number
        // of blocks away. The next pullBlock adopts it.
        engine.requestSeek(0);

        // Walk up to the loop end, asserting position-exactness the whole way:
        // a post-wrap failure then cannot be blamed on the ramp itself.
        bool preWrapExact = true;
        int preWrapShort = 0;
        for (int b = 0; b < BLOCKS_TO_END; ++b) {
            length_t n = engine.pullBlock(L.data(), R.data(), BLOCK);
            if (n != BLOCK) { ++preWrapShort; break; }
            const uint64_t base = (uint64_t)b * BLOCK;
            for (length_t j = 0; j < BLOCK; ++j) {
                if (L[j] != rampVal((long long)(base + j))) {
                    preWrapExact = false;
                    break;
                }
            }
            if (!preWrapExact) break;
        }
        CHECK(preWrapShort == 0,
              "cycle: no dropout while playing up to the loop end");
        CHECK(preWrapExact,
              "cycle: pre-wrap frames decode to their own absolute positions");
        CHECK(engine.currentPosition() == LOOP_END,
              "cycle: playhead sits exactly on the loop end before wrapping");

        // THE WRAP. loopStart is inside the page still held, so this is the
        // fast path of updateFrozenPage — the branch that used to return
        // without re-deriving the cursor.
        length_t nWrap = engine.pullBlock(L.data(), R.data(), BLOCK);
        CHECK(nWrap == BLOCK, "cycle: the wrapping block still produces frames");

        char msg[224];
        std::snprintf(msg, sizeof(msg),
                      "cycle: first post-wrap sample is loopStart's, not the "
                      "pre-wrap offset's (got %.4f, want %.4f, pre-wrap "
                      "offset would give %.4f)",
                      (double)L[0], (double)rampVal(0),
                      (double)rampVal((long long)LOOP_END));
        CHECK(nWrap == BLOCK && L[0] == rampVal(0), msg);

        bool wrapExact = (nWrap == BLOCK);
        for (length_t j = 0; j < nWrap && wrapExact; ++j)
            if (L[j] != rampVal((long long)j)) wrapExact = false;
        CHECK(wrapExact,
              "cycle: the whole post-wrap block replays from loopStart");

        // The product invariant behind all of the above: what is heard and what
        // the playhead reports are the same position.
        CHECK(engine.currentPosition() == BLOCK,
              "cycle: playhead and audio agree after the wrap");

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

    // ------------------------------------------------------------------
    // Position-coded playback under the SCHEDULER: what comes out of
    // pullBlock() must be the audio that belongs at the position the engine
    // says it is at.
    //
    // This is the first time the RT adoption ladder and the demand scheduler
    // are driven together from a unit test. Everything above proves the engine
    // produces frames (amplitude, no dropout, position arithmetic); none of it
    // could catch the engine serving the RIGHT-LOOKING audio from the WRONG
    // position, because a constant tone has nothing to say about where it came
    // from. The staircase does.
    {
        auto src = std::make_shared<PositionCodedComponent>(env);
        src->init();

        // Declared before the engine: AudioEngine only BORROWS the scheduler
        // pointer, so the revalidator has to outlive it.
        CapturePagePool pool(64);
        CaptureRevalidator reval(&pool, 4);

        audio::AudioEngine engine(src, (uint32_t)env.getSRate());
        engine.setScheduler(&reval);   // must precede startReadahead()
        engine.startReadahead();

        // Pull exactly one encoded block at a time: starting from 0 that keeps
        // every window inside one block, which is where the encoding decodes
        // exactly. A window straddling a boundary is DETECTED (low confidence)
        // rather than mis-decoded, and skipped below.
        constexpr length_t BLOCK = (length_t)tw::poscode::kBlockFrames;
        std::vector<float> L((size_t)BLOCK), R((size_t)BLOCK);

        bool audible = false;
        for (int i = 0; i < 1000 && !audible; ++i) {
            length_t n = engine.pullBlock(L.data(), R.data(), BLOCK);
            if (n == BLOCK && !decodeFloats(L.data(), n).silent) audible = true;
            else std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(audible, "coded: scheduler-driven playback produces audio");

        int checked = 0, wrongPosition = 0, ambiguous = 0;
        for (int i = 0; i < 200 && checked < 8; ++i) {
            const uint64_t before = engine.currentPosition();
            const length_t n = engine.pullBlock(L.data(), R.data(), BLOCK);
            if (n != BLOCK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            if (before % (uint64_t)tw::poscode::kBlockFrames != 0) {
                continue;  // window straddles a block: nothing to conclude
            }
            const tw::poscode::Decode d = decodeFloats(L.data(), n);
            if (d.silent) continue;
            if (d.confidence < 100.0) { ++ambiguous; continue; }
            ++checked;
            if ((uint64_t)d.sourceFrame != before) {
                ++wrongPosition;
                printf("     pulled at position %llu but the audio decodes to "
                       "source frame %lld (block %lld, confidence %.1f)\n",
                       (unsigned long long)before, (long long)d.sourceFrame,
                       (long long)d.blockIndex, d.confidence);
            }
        }
        CHECK(checked >= 4,
              "coded: enough aligned blocks pulled to judge position");
        CHECK(wrongPosition == 0,
              "coded: pulled audio decodes to the engine's reported position");
        CHECK(ambiguous == 0,
              "coded: aligned windows decode unambiguously");

        // The scheduler really was the producer — if setScheduler() had been
        // ignored, the readahead would have frozen pages itself and the graph
        // node counter would sit at zero, and the checks above would still
        // pass. They are a position gate, not a plumbing gate; this is the
        // plumbing gate.
        CHECK(reval.graphStats().nodesExecuted > 0,
              "coded: pages came from the demand scheduler, not a local freeze");

        // A live seek must land on the audio OF the new position. This is the
        // assertion a seek-race regression trips: currentPosition() reports the
        // target the moment the RT pull adopts it, so a position-blind test sees
        // a correct seek even when the audio being served still belongs to the
        // old position.
        const uint64_t TARGET = 8ull * (uint64_t)twOutputPage::FRAME_CAPACITY;
        static_assert(twOutputPage::FRAME_CAPACITY % tw::poscode::kBlockFrames == 0,
                      "a page boundary must also be a coded-block boundary, or "
                      "no post-seek window is decodable");
        engine.requestSeek(TARGET);

        bool seekAudioCorrect = false;
        int64_t lastDecoded = -1;
        double lastConfidence = 0.0;
        for (int i = 0; i < 1000 && !seekAudioCorrect; ++i) {
            const uint64_t before = engine.currentPosition();
            const length_t n = engine.pullBlock(L.data(), R.data(), BLOCK);
            if (n != BLOCK || before < TARGET
                || before % (uint64_t)tw::poscode::kBlockFrames != 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            const tw::poscode::Decode d = decodeFloats(L.data(), n);
            if (d.silent || d.confidence < 100.0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            lastDecoded = d.sourceFrame;
            lastConfidence = d.confidence;
            seekAudioCorrect = ((uint64_t)d.sourceFrame == before);
            if (!seekAudioCorrect) break;   // wrong audio: report, do not retry
        }
        if (!seekAudioCorrect) {
            printf("     after seek to %llu: decoded source frame %lld "
                   "(confidence %.1f)\n", (unsigned long long)TARGET,
                   (long long)lastDecoded, lastConfidence);
        }
        CHECK(seekAudioCorrect,
              "coded: after a live seek the audio belongs to the new position");

        engine.stopReadahead();
    }

    printf(failures ? "\n%d FAILURE(S)\n" : "\nall playback tests passed\n",
           failures);
    return failures ? 1 : 0;
}
