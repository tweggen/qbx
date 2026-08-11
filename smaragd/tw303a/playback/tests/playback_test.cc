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
#include <mutex>
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
    // Atomic because the seek-race stress test below deliberately writes it
    // from another thread while a render reads it; a plain offset_t would make
    // the very race under test formally undefined.
    std::atomic<offset_t> pos{0};
    std::atomic<int> renderCalls{0};
    // Widens the window between freezePage_nolock's seekTo(startPos) and the
    // point this render reads `pos` — the window an external seek cascade used
    // to land in. A real component's window is its DSP work; this is the same
    // window, made observable.
    std::atomic<int> renderDelayMs{0};

    bool isSeekable() const override { return true; }
    int seekTo(offset_t p) override { pos.store(p); return 0; }
    void reset() override { pos.store(0); }
    length_t renderFrames(sample_t *out, length_t n, const sample_t *,
                          length_t, idx_t) override {
        renderCalls.fetch_add(1);
        const int delay = renderDelayMs.load();
        if (delay > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        const offset_t at = pos.load();
        for (length_t i = 0; i < n; ++i) {
            out[i] = (sample_t)tw::poscode::sampleAtFrame((int64_t)at + (int64_t)i);
        }
        pos.store(at + (offset_t)n);
        return n;
    }
    // It carries a play cursor written by seekTo() and advanced by
    // renderFrames() — the exact shape usesSerialCursor() describes, so freezes
    // of it must serialize on cursorMutex_ like twWavInput's do. Without this
    // the component would race ITSELF and the seek stress test below could not
    // attribute a wrong page to the external seek.
    bool usesSerialCursor() const override { return true; }
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

    // ------------------------------------------------------------------
    // The seek/freeze race, head-on: freezes running while the PUBLIC seek
    // path is hammered.
    //
    // A page freeze positions its component itself — freezePage_nolock does
    // reset()/restore, then seekTo(page->startPosition), then renderFrames() —
    // and serializes that on cursorMutex_. An outside seek takes a DIFFERENT
    // lock, so one landing between that seekTo and the render rewrites the
    // cursor the render is about to read: the whole 65536-frame page comes out
    // as the audio of the SEEK TARGET, cached under (and served for) its
    // original startPos, stamped valid and current. Nothing downstream can
    // notice — the page looks perfectly well-formed.
    //
    // Which is why this assertion needs position-coded content: it decodes
    // every page that came out of the storm and demands that each one carry the
    // audio of ITS OWN startPosition. AudioEngine::seekTo() is the hammer
    // because it is the seek entry point playback actually used, and the one
    // that used to cascade into the graph.
    {
        auto src = std::make_shared<PositionCodedComponent>(env);
        src->init();
        // Widen the seekTo→render window. Without this the race is real but
        // narrow (a page render is a few ms); with it a hammering seeker lands
        // inside essentially every freeze.
        src->renderDelayMs.store(2);

        // The engine is here only to own the seek entry point. No readahead is
        // started: this test drives the freezes itself, so nothing else
        // competes for pages and every produced page is accounted for.
        audio::AudioEngine engine(src, (uint32_t)env.getSRate());

        constexpr int FREEZERS = 4;
        constexpr int ROUNDS   = 60;
        const length_t FULL = (length_t)twOutputPage::FRAME_CAPACITY;
        static_assert(twOutputPage::FRAME_CAPACITY % tw::poscode::kBlockFrames == 0,
                      "a page must span whole coded blocks or it cannot be "
                      "decoded window by window");

        // Distinct page positions in play. Capped so every probed window falls
        // inside the encoding's candidate range: decodeBuffer() only sweeps
        // kDefaultBlocks bins, so a window past that decodes to the wrong block
        // with low confidence — an artefact of the fixture's extent, not of the
        // engine, and it would make the storm unreadable.
        constexpr int SPREAD =
            (int)((tw::poscode::kDefaultBlocks * tw::poscode::kBlockFrames)
                  / (int64_t)twOutputPage::FRAME_CAPACITY);
        static_assert(SPREAD >= 4, "too few in-range page positions to storm");

        std::atomic<bool> stop{false};
        std::mutex collectMutex;
        std::vector<std::shared_ptr<twOutputPage>> produced;

        std::thread seeker([&] {
            uint64_t k = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                // Sweep the same page positions the freezers use, so a leaked
                // cascade lands on a plausible-but-wrong position rather than
                // somewhere obviously out of range.
                engine.seekTo((++k % SPREAD) * (uint64_t)FULL);
            }
        });

        std::vector<std::thread> freezers;
        for (int t = 0; t < FREEZERS; ++t) {
            freezers.emplace_back([&, t] {
                std::vector<std::shared_ptr<twOutputPage>> mine;
                for (int r = 0; r < ROUNDS; ++r) {
                    const offset_t startPos =
                        (offset_t)((r * FREEZERS + t) % SPREAD) * (offset_t)FULL;
                    auto p = src->freezePage(startPos, nullptr, 0, FULL,
                                             env.getSRate(), nullptr);
                    if (p && p->validAspects != 0 && p->validFrames > 0)
                        mine.push_back(p);
                    // Force a genuine render next round instead of a cache hit:
                    // a cached page proves nothing about the race.
                    src->bumpContentEpoch();
                }
                std::lock_guard<std::mutex> lk(collectMutex);
                for (auto &p : mine) produced.push_back(std::move(p));
            });
        }
        for (auto &th : freezers) th.join();
        stop.store(true, std::memory_order_relaxed);
        seeker.join();

        // Decode two windows per page — the head, and one a third of the way in
        // — so a page that is only partly displaced is caught too.
        const int64_t PROBES[] = { 0, 8 * (int64_t)tw::poscode::kBlockFrames };
        int decoded = 0, displaced = 0, ambiguous = 0;
        int64_t firstBadAt = -1, firstBadTo = -1, firstBadWindow = -1;

        for (const auto &p : produced) {
            for (int64_t off : PROBES) {
                if (off + (int64_t)tw::poscode::kBlockFrames > (int64_t)p->validFrames)
                    continue;
                const tw::poscode::Decode d = decodeFloats(
                    p->samples.data() + off, (int64_t)tw::poscode::kBlockFrames);
                if (d.silent || d.confidence < 100.0) { ++ambiguous; continue; }
                ++decoded;
                const int64_t expect = (int64_t)p->startPosition + off;
                if (d.sourceFrame != expect) {
                    ++displaced;
                    if (firstBadAt < 0) {
                        firstBadAt     = expect;
                        firstBadTo     = d.sourceFrame;
                        firstBadWindow = off;
                    }
                }
            }
        }

        if (displaced) {
            printf("     %d of %d decoded windows carry the wrong position; "
                   "first: page window at source frame %lld decodes to %lld "
                   "(probe offset %lld)\n",
                   displaced, decoded, (long long)firstBadAt,
                   (long long)firstBadTo, (long long)firstBadWindow);
        }
        CHECK(src->renderCalls.load() >= FREEZERS * ROUNDS / 2,
              "seek storm: the freezers really rendered (not served from cache)");
        CHECK(decoded >= FREEZERS * ROUNDS,
              "seek storm: enough pages decoded to judge");
        CHECK(ambiguous == 0, "seek storm: every probed window decodes cleanly");
        CHECK(displaced == 0,
              "seek storm: every frozen page carries the audio of its OWN "
              "start position");
    }

    printf(failures ? "\n%d FAILURE(S)\n" : "\nall playback tests passed\n",
           failures);
    return failures ? 1 : 0;
}
