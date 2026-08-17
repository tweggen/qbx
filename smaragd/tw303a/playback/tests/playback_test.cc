// tw/playback module test: AudioEngine page adoption under mid-playback edits
// (proposal 16). An edit bumps the producer's content epoch; the audio thread
// must keep serving the stale-but-consistent pre-edit pages as a fallback —
// never silence — until the readahead re-freezes them, and must switch to the
// new content as soon as it lands. Also covers the component-level mechanism:
// a placeholder replacing a stale-frozen page keeps it reachable via
// stalePredecessor until the replacement is stamped frozen.
#include "tw/playback/audio_engine.h"
#include "tw/playback/twspeaker.h"   // twmonitor:: — the device rule, as a pure function
#include "tw/graph/twcomponent.h"
#include "tw/graph/tw303aenv.h"
#include "tw/pages/tw_output_page.h"
#include "tw/pages/capture_page_pool.h"
#include "tw/schedule/capture_revalidator.h"
#include "tw/core/position_code.h"
// Proposal 21 L1a: the live lane engine.
#include "tw/playback/twliveclock.h"
#include "tw/playback/twliveplan.h"
#include "tw/playback/twlivepump.h"
#include "tw/playback/twlivering.h"
#include "tw/devices/capture_backend.h"
#include "tw/events/tweventsource.h"
#include "tw/mix/twmixer.h"
#include "tw/mix/twrewire.h"
#include "tw/plugins/twplugindescriptor.h"
#include "tw/plugins/twplugininsert.h"
#include "tw/plugins/twpluginslotproc.h"
#include "tw/graph/twlatch.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>
#include <cstdlib>
#include <map>
#include <vector>

using audio::twPluginInsert;
using audio::twPluginSlotProcessor;
using audio::twPluginIoLayout;
using audio::twPluginDescriptor;
using audio::pluginRegistry;
using audio::twPlugin;

static int failures = 0;
#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (cond) { printf("ok   %s\n", msg); }                             \
        else      { printf("FAIL %s\n", msg); ++failures; }                 \
    } while (0)

// pullBlock() takes N planar buffers since proposal 36 B5. Almost every case
// below wants exactly two, so the L/R shape stays here as a test-local shim
// rather than as an engine API that pins the sink at stereo.
static length_t pullLR(audio::AudioEngine &e, std::vector<float> &L,
                       std::vector<float> &R, length_t n)
{
    float *chans[2] = { L.data(), R.data() };
    return e.pullBlock(chans, 2, n);
}

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

// A WIDE tone whose declared width can be changed at runtime, for AC B4.5's
// RT half. `widestPageSeen` records the widest page it was ever asked to fill,
// so a test can prove the re-freeze really happened at the new width rather
// than merely that audio came back.
class WidthTone : public twComponent {
public:
    explicit WidthTone(tw303aEnvironment &e) : twComponent(e) {}
    std::atomic<int> width{2};
    std::atomic<float> amp{0.25f};
    std::atomic<int> widestPageSeen{0};
    // With `ladder` set, channel c carries amp/(c+1) — a 6 dB step per channel,
    // so a pull can tell WHICH channel it got rather than only that it got one
    // (proposal 36 B5). Off by default: the B4 cases below assert amp exactly.
    std::atomic<bool> ladder{false};

    idx_t getOutputChannels() const override { return (idx_t)width.load(); }

    bool isSeekable() const override { return true; }
    int seekTo(offset_t) override { return 0; }
    void reset() override {}

    length_t renderPageWide(twOutputPage &page, length_t frames,
                            const sample_t *, length_t) override {
        length_t n = frames;
        if (n > (length_t)page.channelFrames()) n = (length_t)page.channelFrames();
        const int nCh = (int)page.channels();
        int seen = widestPageSeen.load();
        while (nCh > seen && !widestPageSeen.compare_exchange_weak(seen, nCh)) {}
        const float a = amp.load();
        const bool  lad = ladder.load();
        for (idx_t c = 0; c < (idx_t)nCh; ++c) {
            sample_t *dst = page.channelPtr(c);
            const float v = lad ? (a / (float)(c + 1)) : a;
            for (length_t i = 0; i < n; ++i) dst[i] = v;
        }
        return n;
    }
    // The narrow degradation (proposal 36 §7 trap 18).
    length_t renderFrames(sample_t *out, length_t n, const sample_t *,
                          length_t, idx_t) override {
        const float a = amp.load();
        for (length_t i = 0; i < n; ++i) out[i] = a;
        return n;
    }
    void createOutputLatches() override {}
    idx_t getNInputs() const override { return 0; }
    idx_t getNOutputs() const override { return 1; }
    const char *getInputName(idx_t) const override { return nullptr; }
    const char *getOutputName(idx_t) const override { return "widetone"; }
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


// ---------------------------------------------------------------------------
// Proposal 21 L1a fixtures: everything the live-lane blocks in main() need.
// ---------------------------------------------------------------------------

// The app services twSpeaker needs, as the two-lane transitions exercise them.
class TestPlaybackContext : public audio::PlaybackContext {
public:
    std::shared_ptr<twComponent> root;
    std::atomic<std::uint64_t>   published{ 0 };
    std::atomic<int>             publishCount{ 0 };

    std::shared_ptr<twComponent> rootComponent() override { return root; }
    std::uint64_t locatorPosition() override { return 0; }
    void publishPosition(std::uint64_t absPos) override
    {
        published.store(absPos, std::memory_order_relaxed);
        publishCount.fetch_add(1, std::memory_order_relaxed);
    }
};

// A WIDE source whose sample is a pure function of (channel, absolute position)
// — "position-encoded" in the only sense the sample-exactness claim needs: two
// renderers that agree on where they are produce the same numbers, and two that
// do not cannot possibly agree by accident.
class PosSource : public twComponent {
public:
    PosSource(tw303aEnvironment &e, idx_t ch) : twComponent(e), channels_(ch) {}

    static float value(idx_t c, offset_t p)
    {
        return (float)(c + 1) * (float)((p % 17) + 1) * 0.01f;
    }

    idx_t getOutputChannels() const override { return channels_; }
    bool  isSeekable() const override { return true; }
    int   seekTo(offset_t p) override { pos_ = p; return 0; }
    void  reset() override { pos_ = 0; }

    length_t renderPageWide(twOutputPage &page, length_t frames, const sample_t *,
                            length_t) override
    {
        length_t n = frames;
        if (n > (length_t)page.channelFrames()) n = (length_t)page.channelFrames();
        for (idx_t c = 0; c < (idx_t)page.channels(); ++c) {
            sample_t *dst = page.channelPtr(c);
            for (length_t i = 0; i < n; ++i)
                dst[i] = value(c, page.startPosition + (offset_t)i);
        }
        return n;
    }
    // The narrow degradation (proposal 36 §7 trap 18): it must exist or the base
    // renderFrames/calcOutputTo pair recurses until the stack ends.
    length_t renderFrames(sample_t *out, length_t n, const sample_t *, length_t,
                          idx_t) override
    {
        for (length_t i = 0; i < n; ++i) out[i] = value(0, pos_ + (offset_t)i);
        pos_ += (offset_t)n;
        return n;
    }
    // It is PULLED THROUGH A LATCH by the insert above it, so it must publish
    // one (an empty override leaves linkOutput(0) null and the insert silent).
    void createOutputLatches() override
    {
        pOutputLatches_.resize(1);
        pOutputLatches_[0] =
            std::make_shared<twStreamingLatch>(shared_from_this(), 0, 4096);
    }
    bool usesSerialCursor() const override { return true; }
    idx_t getNInputs() const override { return 0; }
    idx_t getNOutputs() const override { return 1; }
    const char *getInputName(idx_t) const override { return nullptr; }
    const char *getOutputName(idx_t) const override { return "pos"; }

private:
    idx_t    channels_;
    offset_t pos_ = 0;
};

// The same material, as the live lane's INPUT. `pos` is what makes it a pure
// function of position rather than of call order — which is exactly the
// property a device ring does not have and does not need.
class PosInput : public twLiveInputSource {
public:
    std::size_t pull(float *const *out, std::size_t channels, std::size_t frames,
                     offset_t pos) override
    {
        for (std::size_t c = 0; c < channels; ++c)
            for (std::size_t i = 0; i < frames; ++i)
                out[c][i] = PosSource::value((idx_t)c, pos + (offset_t)i);
        return frames;
    }
};

// The review-fix fixture. A pure function of position that NEVER returns zero,
// so a zero in a recording is unambiguously a GAP and not a quiet sample. The
// period is 1000 frames, which is coprime with every block size the variable-
// grid tests use, so no partition can accidentally look right.
inline float rampAt(std::int64_t p)
{
    const std::int64_t m = ((p % 1000) + 1000) % 1000;
    return 0.25f + (float)m * 0.0005f;
}

class RampInput : public twLiveInputSource {
public:
    std::size_t pull(float *const *out, std::size_t channels, std::size_t frames,
                     offset_t pos) override
    {
        for (std::size_t c = 0; c < channels; ++c)
            for (std::size_t i = 0; i < frames; ++i)
                out[c][i] = rampAt((std::int64_t)pos + (std::int64_t)i);
        return frames;
    }
};

class ConstInput : public twLiveInputSource {
public:
    explicit ConstInput(float v) : v_(v) {}
    std::size_t pull(float *const *out, std::size_t channels, std::size_t frames,
                     offset_t) override
    {
        for (std::size_t c = 0; c < channels; ++c)
            std::fill(out[c], out[c] + frames, v_);
        return frames;
    }
private:
    float v_;
};

// One note, as a twEventSource. Enough to tell "this source reached the
// instrument" from "it did not", which is the whole of the stopped-transport
// feed gate.
class OneNoteSource : public twEventSource {
public:
    OneNoteSource(std::int64_t start, std::int64_t len, std::int16_t key,
                  std::int32_t id)
        : start_(start), len_(len), key_(key), id_(id) {}

    void collect(std::int64_t startPos, std::int64_t len,
                 twEventBlock &out) const override
    {
        out.clear();
        const std::int64_t end = start_ + len_;
        if (start_ >= startPos && start_ < startPos + len) {
            twEvent e;
            e.time     = start_ - startPos;
            e.kind     = twEventKind::NoteOn;
            e.channel  = 0;
            e.key      = key_;
            e.noteId   = id_;
            e.value    = 100.0;      // MIDI domain; the processor normalizes
            e.duration = 0;
            out.events.push_back(e);
        } else if (start_ < startPos && end > startPos) {
            twHeldNote h;
            h.channel  = 0;
            h.key      = key_;
            h.noteId   = id_;
            h.velocity = 100.0;
            h.start    = start_;
            h.duration = 0;
            out.chase.notes.push_back(h);
        }
        if (end >= startPos && end < startPos + len) {
            twEvent e;
            e.time    = end - startPos;
            e.kind    = twEventKind::NoteOff;
            e.channel = 0;
            e.key     = key_;
            e.noteId  = id_;
            out.events.push_back(e);
        }
        out.sortEvents();
    }

private:
    std::int64_t start_, len_;
    std::int16_t key_;
    std::int32_t id_;
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
            length_t n = pullLR(engine, L, R, BLOCK);
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

        length_t n = pullLR(engine, L, R, BLOCK);
        CHECK(n == BLOCK,
              "pullBlock immediately after an edit still produces frames");
        CHECK(n == BLOCK && L[0] == 0.25f && L[BLOCK - 1] == 0.25f,
              "fallback frames carry the consistent PRE-edit content");

        // Keep pulling at roughly realtime pace: the post-edit content must
        // arrive, and no pull in between may come up short (silence).
        src->renderDelayMs.store(0);
        bool freshHeard = false, dropout = false;
        for (int i = 0; i < 500 && !freshHeard && !dropout; ++i) {
            length_t got = pullLR(engine, L, R, BLOCK);
            if (got != BLOCK) { dropout = true; break; }
            if (L[0] == 0.5f) { freshHeard = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(!dropout, "no dropout between the edit and the re-freeze");
        CHECK(freshHeard, "post-edit content becomes audible after re-freeze");

        engine.stopReadahead();
    }

    // ------------------------------------------------------------------
    // AC B4.5 (proposal 36), THE RT HALF: a stale page of the WRONG WIDTH must
    // be a MISS on the audio thread, never audio.
    //
    // Why this half had to wait for B4. §4.5's rule is wired at three places —
    // twLevelProbe (proven in metering_test since B2), and the fast path plus
    // both stale fallbacks in AudioEngine::updateFrozenPage. The probe half
    // could be forced with a synthetic component; the RT half could not, because
    // "a page reaches the RT callback" needs a real engine, and until B4 nothing
    // the engine could be pointed at was ever wider than one channel. It is now.
    //
    // WHAT MAKES THIS A REAL FORCING, not a staged one: the width changes with
    // NO content-epoch bump. So every cached page is still CURRENT by the epoch
    // rule and by validAspects — proposal 16's stale fallback would happily
    // serve it — and the width check is the only thing standing between the
    // audio thread and reading channelPtr(1) of a page that has one channel.
    {
        auto src = std::make_shared<WidthTone>(env);
        src->width.store(2);
        src->init();

        audio::AudioEngine engine(src, (uint32_t)env.getSRate());
        engine.startReadahead();

        constexpr length_t BLOCK = 512;
        std::vector<float> L(BLOCK), R(BLOCK);

        bool audible = false;
        for (int i = 0; i < 500 && !audible; ++i) {
            length_t n = pullLR(engine, L, R, BLOCK);
            if (n == BLOCK && L[0] == 0.25f) { audible = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(audible, "width: a 2-channel graph plays");

        // THE WIDTH CHANGE, with no epoch bump: the frozen pages stay valid and
        // current, and only their geometry is now wrong.
        src->width.store(4);

        bool servedOldWidth = false, sawSilence = false;
        for (int i = 0; i < 40; ++i) {
            std::fill(L.begin(), L.end(), -1.0f);
            length_t n = pullLR(engine, L, R, BLOCK);
            if (n == BLOCK && L[0] == 0.25f) { servedOldWidth = true; break; }
            if (n == 0 || L[0] == 0.0f) sawSilence = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        CHECK(!servedOldWidth,
              "AC B4.5 (RT): a page whose width no longer matches its producer is "
              "NOT served to the audio thread");
        CHECK(sawSilence,
              "AC B4.5 (RT): the RT path falls back to silence for that page");

        // And it is not a permanent poisoning: bumping the epoch lets the
        // readahead re-freeze at the new width, and audio returns.
        src->bumpContentEpoch();
        bool recovered = false;
        for (int i = 0; i < 500 && !recovered; ++i) {
            length_t n = pullLR(engine, L, R, BLOCK);
            if (n == BLOCK && L[0] == 0.25f) { recovered = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(recovered,
              "AC B4.5 (RT): re-freezing at the new width restores audio");
        CHECK(src->widestPageSeen.load() == 4,
              "…and the pages the readahead froze really were 4 channels wide");

        engine.stopReadahead();
    }

    // ------------------------------------------------------------------
    // Proposal 36 B5 — pullBlock() serves N CHANNELS, and applies the §4.4
    // clamp when asked for more than the page has.
    //
    // This is the playback half of "the sink goes wide". Until B5 pullBlock
    // took (outL, outR) and filled BOTH from channelPtr(0), so a 4-channel
    // graph and a mono one produced the same two buffers. The three assertions
    // below separate the three things that could still be wrong: that the
    // channels arrive at all, that they arrive in the RIGHT ORDER (a ladder,
    // not merely "different"), and that a request wider than the page degrades
    // by the clamp rather than by reading out of bounds.
    {
        auto src = std::make_shared<WidthTone>(env);
        src->width.store(4);
        src->ladder.store(true);          // channel c == amp/(c+1)
        src->init();

        audio::AudioEngine engine(src, (uint32_t)env.getSRate());
        engine.startReadahead();

        CHECK(engine.graphChannels() == 4,
              "B5: the engine reports the graph's width (4)");

        constexpr length_t BLOCK = 512;
        // Ask for SIX buffers from a four-channel page: channels 4 and 5 must
        // come back as channel 3 (the clamp), not as garbage and not as zero.
        constexpr std::size_t ASK = 6;
        std::vector<std::vector<float>> bufs(ASK, std::vector<float>(BLOCK, -1.0f));
        std::vector<float *> chans(ASK);
        for (std::size_t c = 0; c < ASK; ++c) chans[c] = bufs[c].data();

        bool audible = false;
        for (int i = 0; i < 500 && !audible; ++i) {
            length_t n = engine.pullBlock(chans.data(), ASK, BLOCK);
            if (n == BLOCK && bufs[0][0] == 0.25f) { audible = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(audible, "B5: a 4-channel graph plays");

        const float want[4] = { 0.25f, 0.125f, 0.25f / 3.0f, 0.0625f };
        bool ladderOk = audible;
        for (std::size_t c = 0; c < 4 && ladderOk; ++c)
            for (length_t i = 0; i < BLOCK && ladderOk; ++i)
                ladderOk = std::fabs(bufs[c][i] - want[c]) < 1e-6f;
        CHECK(ladderOk,
              "B5: destination channel c carries PAGE channel c (a 6 dB ladder, "
              "in order) — not channel 0 four times");

        bool clampOk = audible;
        for (std::size_t c = 4; c < ASK && clampOk; ++c)
            for (length_t i = 0; i < BLOCK && clampOk; ++i)
                clampOk = std::fabs(bufs[c][i] - want[3]) < 1e-6f;
        CHECK(clampOk,
              "B5: asking for more channels than the page has yields its LAST "
              "channel (the §4.4 clamp), never an out-of-bounds read");

        // …and a MONO page asked for two buffers gives the same audio on both:
        // "mono plays on every channel", which is what keeps a mono project
        // audible on a stereo device.
        src->ladder.store(false);
        src->width.store(1);
        src->bumpContentEpoch();
        std::vector<float> L(BLOCK, -1.0f), R(BLOCK, -1.0f);
        bool mono = false;
        for (int i = 0; i < 500 && !mono; ++i) {
            length_t n = pullLR(engine, L, R, BLOCK);
            if (n == BLOCK && L[0] == 0.25f) { mono = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        bool bothOk = mono;
        for (length_t i = 0; i < BLOCK && bothOk; ++i) bothOk = (L[i] == R[i]);
        CHECK(mono && bothOk,
              "B5: a width-1 graph fans out to every destination channel");

        engine.stopReadahead();
    }

    // ------------------------------------------------------------------
    // Proposal 36 B5 — THE DEVICE MONITORING RULE, asserted without a device.
    //
    //     L = ch0;  R = (projectWidth >= 2) ? ch1 : ch0
    //
    // and that pair meets the device's channel count as it always has. The rule
    // is a pure function in twspeaker.h for exactly this reason: "a width-6
    // graph reaching a stereo device yields exactly ch0 and ch1" is a one-line
    // assertion here and would need a real backend, a PlaybackContext and a
    // paced pump anywhere else.
    //
    // What this stops: a future refactor quietly fanning project channel 2 into
    // the right output, or a "positional" mapping that silently starts feeding
    // a 6-channel device six project channels. Monitoring is stereo, on
    // purpose; the DROPPING of channels 2..N is the decision, not an accident,
    // and it is confined to the device — a render still writes all N (that is
    // RenderSession, which shares no code with this, and render_test's
    // 6-channel case is where it is asserted).
    {
        const std::size_t N = 8;
        std::vector<std::vector<float>> src(6, std::vector<float>(N));
        std::vector<const float *> ptr(6);
        for (std::size_t c = 0; c < 6; ++c) {
            for (std::size_t i = 0; i < N; ++i)
                src[c][i] = 0.1f * (float)(c + 1);      // channel c == 0.1*(c+1)
            ptr[c] = src[c].data();
        }

        CHECK(twmonitor::pullChannels(1) == 1 &&
              twmonitor::pullChannels(2) == 2 &&
              twmonitor::pullChannels(6) == 2 &&
              twmonitor::pullChannels(8) == 2,
              "B5 device rule: monitoring pulls 1 channel from a mono project "
              "and exactly 2 from every wider one");

        // width 6 -> a STEREO device: L = ch0 (0.1), R = ch1 (0.2). Channels
        // 2..5 (0.3..0.6) must appear nowhere.
        std::vector<float> dev2(N * 2, -1.0f);
        twmonitor::interleave(dev2.data(), N, 2, ptr.data(), twmonitor::pullChannels(6));
        bool ok2 = true;
        for (std::size_t i = 0; i < N && ok2; ++i)
            ok2 = (dev2[i * 2] == 0.1f) && (dev2[i * 2 + 1] == 0.2f);
        CHECK(ok2,
              "AC B5 device rule: a 6-channel project on a STEREO device is "
              "exactly ch0/ch1; channels 2..5 are dropped at the device");

        // width 6 -> a SIX-channel device: still the pair, alternating, which
        // is what the device mapping has always done. NOT ch0..ch5.
        std::vector<float> dev6(N * 6, -1.0f);
        twmonitor::interleave(dev6.data(), N, 6, ptr.data(), twmonitor::pullChannels(6));
        bool ok6 = true;
        for (std::size_t i = 0; i < N && ok6; ++i)
            for (unsigned c = 0; c < 6 && ok6; ++c)
                ok6 = (dev6[i * 6 + c] == ((c % 2 == 0) ? 0.1f : 0.2f));
        CHECK(ok6,
              "AC B5 device rule: a 6-channel project on a 6-channel device is "
              "STILL the ch0/ch1 pair — monitoring is stereo, so channel 2 does "
              "not reach output 2");

        // width 1 -> stereo device: standard mono-to-stereo, L == R == ch0.
        std::vector<float> devMono(N * 2, -1.0f);
        twmonitor::interleave(devMono.data(), N, 2, ptr.data(), twmonitor::pullChannels(1));
        bool okM = true;
        for (std::size_t i = 0; i < N && okM; ++i)
            okM = (devMono[i * 2] == 0.1f) && (devMono[i * 2 + 1] == 0.1f);
        CHECK(okM, "AC B5 device rule: a MONO project is L = R = ch0");

        // width 2 -> stereo device: itself.
        std::vector<float> devSt(N * 2, -1.0f);
        twmonitor::interleave(devSt.data(), N, 2, ptr.data(), twmonitor::pullChannels(2));
        bool okS = true;
        for (std::size_t i = 0; i < N && okS; ++i)
            okS = (devSt[i * 2] == 0.1f) && (devSt[i * 2 + 1] == 0.2f);
        CHECK(okS, "AC B5 device rule: a STEREO project is L = ch0, R = ch1");
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
            length_t n = pullLR(engine, L, R, BLOCK);
            if (n == BLOCK && L[0] == 0.25f) audible = true;
            else std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        for (int i = 0; i < 8; ++i) pullLR(engine, L, R, BLOCK);
        CHECK(audible && engine.currentPosition() > 0,
              "seek: playback running and advanced before the seek");

        // Forward live seek, page-aligned and far past the readahead frontier.
        const uint64_t TARGET = 4ull * (uint64_t)twOutputPage::FRAME_CAPACITY;
        engine.requestSeek(TARGET);
        // The very next pull adopts it (the RT pull is the sole writer of
        // currentPos_), even before any page at TARGET is frozen.
        pullLR(engine, L, R, BLOCK);
        uint64_t after = engine.currentPosition();
        CHECK(after >= TARGET && after < TARGET + 4 * BLOCK,
              "seek: RT pull adopts the requested position");

        // Playback resumes from the new position (tone flows again).
        bool resumed = false;
        for (int i = 0; i < 500 && !resumed; ++i) {
            length_t n = pullLR(engine, L, R, BLOCK);
            if (n == BLOCK && L[0] == 0.25f) resumed = true;
            else std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(resumed && engine.currentPosition() > TARGET,
              "seek: playback resumes and advances from the sought position");

        // A backward live seek repositions just the same.
        engine.requestSeek(0);
        pullLR(engine, L, R, BLOCK);
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
            length_t n = pullLR(engine, L, R, BLOCK);
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
            length_t n = pullLR(engine, L, R, BLOCK);
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
        length_t nWrap = pullLR(engine, L, R, BLOCK);
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
                  oldPage->channelPtr(0)[0] == 0.25f,
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
                  freshPage->channelPtr(0)[0] == 0.75f,
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
            length_t n = pullLR(engine, L, R, BLOCK);
            if (n == BLOCK && !decodeFloats(L.data(), n).silent) audible = true;
            else std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(audible, "coded: scheduler-driven playback produces audio");

        int checked = 0, wrongPosition = 0, ambiguous = 0;
        for (int i = 0; i < 200 && checked < 8; ++i) {
            const uint64_t before = engine.currentPosition();
            const length_t n = pullLR(engine, L, R, BLOCK);
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
            const length_t n = pullLR(engine, L, R, BLOCK);
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
                    p->channelPtr(0) + off, (int64_t)tw::poscode::kBlockFrames);
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

    // ==================================================================
    // PROPOSAL 21 L1a — the live lane engine.
    //
    // AC3 (the ring gate + the master-shape precondition), AC1 (the speaker's
    // two-lane transitions), AC2 (the synthetic-plan harness) and AC4 (the
    // ownership guard). The order matters in one respect only: the AC2 harness
    // MARKS ITS THREAD as the live pump, and that marker is sticky, so it runs
    // on a thread of its own and every frozen render it is compared against is
    // done here, on main, where the render policy is still Any.
    // ==================================================================

    // --- AC3a: the RT sum, as a pure function --------------------------
    {
        printf("\n=== 21 L1a AC3: the epoch-gated ring sum ===\n");

        constexpr std::size_t N = 8;
        std::vector<float> ring(2 * N, 0.5f);
        twLiveRingEntry e;
        e.startPos  = 1024;
        e.frames    = (std::uint32_t)N;
        e.channels  = 2;
        e.data      = ring.data();
        e.stride    = N;
        e.playing   = true;

        std::vector<float> a(N, 0.0f), b(N, 0.0f);
        float *outs[2] = { a.data(), b.data() };
        auto clear = [&] { std::fill(a.begin(), a.end(), 0.0f);
                           std::fill(b.begin(), b.end(), 0.0f); };

        twLiveMixState st;   // fadeFrames 0 == a plain sum

        // (1) POSITION. The block was rendered for frame 1024; the RT is on 2048.
        {
            clear();
            twLiveMixGate g; g.wantPos = 2048; g.haveRoot = true; g.rootEpoch = 100;
            const twLiveMixOutcome oc = twlive::mixRing(outs, 2, N, e, g, st);
            CHECK(oc == twLiveMixOutcome::PositionMismatch,
                  "L1a AC3: a ring entry stamped for another frame is NOT summed");
            CHECK(a[0] == 0.0f && b[0] == 0.0f,
                  "L1a AC3: ...and the output is left silent, not approximated");
        }

        // (2) ARM. flipEpoch 100: a root page older than the wiring bump still
        //     CONTAINS the armed track, so summing would double it.
        {
            clear();
            e.flipEpoch = 100; e.flipEpochPrime = 0;
            twLiveMixGate g; g.wantPos = 1024; g.haveRoot = true; g.rootEpoch = 99;
            CHECK(twlive::mixRing(outs, 2, N, e, g, st) ==
                      twLiveMixOutcome::EpochNotYetFlipped,
                  "L1a AC3 arm: rootEpoch < flipEpoch => NOT summed");
            CHECK(a[0] == 0.0f, "L1a AC3 arm: ...silent while the flip is pending");

            clear();
            g.rootEpoch = 100;
            CHECK(twlive::mixRing(outs, 2, N, e, g, st) == twLiveMixOutcome::Summed,
                  "L1a AC3 arm: rootEpoch == flipEpoch => summed");
            CHECK(a[0] == 0.5f && b[0] == 0.5f,
                  "L1a AC3 arm: ...and every channel carries the ring");
        }

        // (3) DISARM (the mirror). flipEpoch' 200: a root page older than the
        //     re-wiring still LACKS the track, so the ring must keep filling
        //     the hole until the re-summed page lands.
        {
            clear();
            e.flipEpoch = 100; e.flipEpochPrime = 200;
            twLiveMixGate g; g.wantPos = 1024; g.haveRoot = true; g.rootEpoch = 150;
            CHECK(twlive::mixRing(outs, 2, N, e, g, st) == twLiveMixOutcome::Summed,
                  "L1a AC3 disarm: rootEpoch < flipEpoch' => STILL summed");
            CHECK(a[0] == 0.5f, "L1a AC3 disarm: ...the hole is filled");

            clear();
            g.rootEpoch = 200;
            CHECK(twlive::mixRing(outs, 2, N, e, g, st) == twLiveMixOutcome::EpochResummed,
                  "L1a AC3 disarm: the re-summed page lands => the ring stops");
            CHECK(a[0] == 0.0f, "L1a AC3 disarm: ...and stops exactly then");
        }

        // (4) STOPPED: no root page at all, so out = ring whatever the epochs.
        {
            clear();
            e.flipEpoch = 100; e.flipEpochPrime = 0;
            twLiveMixGate g; g.wantPos = 1024; g.haveRoot = false; g.rootEpoch = 0;
            CHECK(twlive::mixRing(outs, 2, N, e, g, st) == twLiveMixOutcome::Summed,
                  "L1a AC3 stopped: with no root page the ring is the output");
        }

        // (5) THE CROSSFADE. 8 frames of ramp over an 8-frame block: the first
        //     sample is 0 and the last is 7/8 of the entry.
        {
            clear();
            e.flipEpoch = 0; e.flipEpochPrime = 0;
            twLiveMixState fade; fade.fadeFrames = (std::uint32_t)N;
            twLiveMixGate g; g.wantPos = 1024; g.haveRoot = false;
            twlive::mixRing(outs, 2, N, e, g, fade);
            CHECK(a[0] == 0.0f, "L1a AC3 fade-in: the first frame is silent");
            CHECK(std::fabs(a[N - 1] - 0.5f * 7.0f / 8.0f) < 1e-6f,
                  "L1a AC3 fade-in: the ramp is linear across the block");
            CHECK(fade.fadeDone == (std::uint32_t)N,
                  "L1a AC3 fade-in: the ramp is complete after one block");
            clear();
            fade.fadingOut = true; fade.fadeDone = 0;
            twlive::mixRing(outs, 2, N, e, g, fade);
            CHECK(a[0] == 0.5f && std::fabs(a[N - 1] - 0.5f / 8.0f) < 1e-6f,
                  "L1a AC3 fade-out: the mirror ramp");
        }

        // (6) The SPSC ring itself: FIFO, stamped, and full means DROP.
        {
            twLiveMixRing r;
            r.reset(1, 4, 2);
            float *w0 = r.beginWrite();
            CHECK(w0 != nullptr, "L1a AC3 ring: beginWrite yields a block");
            for (int i = 0; i < 4; ++i) w0[i] = 1.0f;
            r.commit(0, 4, 0, 0, true);
            float *w1 = r.beginWrite();
            CHECK(w1 != nullptr && w1 != w0, "L1a AC3 ring: the second slot differs");
            r.commit(4, 4, 0, 0, true);
            CHECK(r.beginWrite() == nullptr && r.overruns() == 1,
                  "L1a AC3 ring: a full ring DROPS and counts, never blocks or grows");
            twLiveRingEntry got;
            CHECK(r.peek(got) && got.startPos == 0, "L1a AC3 ring: FIFO order");
            CHECK(r.pending() == 2, "L1a AC3 ring: pending() counts what is readable");
            r.pop();
            CHECK(r.peek(got) && got.startPos == 4, "L1a AC3 ring: ...and it advances");
            // The drop-past is the READER's, not a separate dropBefore(): a
            // stream consumer that is at frame 8 walks past everything behind
            // it on its way to what it wants (review fix 1).
            {
                std::vector<float> o(4, 0.0f);
                float *outs[1] = { o.data() };
                twLiveMixReader   rd;
                twLiveMixState    fs;
                twLiveMixGate     g; g.wantPos = 8; g.haveRoot = true;   // positional authority
                twLiveStreamStats st;
                twlive::mixStream(outs, 1, 4, 8, g, r, rd, fs, st);
                CHECK(st.dropped == 1 && st.starved == 1 && !r.peek(got),
                      "L1a AC3 ring: a consumer past an entry DROPS it and counts");
            }
        }
    }

    // --- AC3b: the master-shape precondition ---------------------------
    {
        printf("\n=== 21 L1a AC3: the master-shape precondition ===\n");

        // Constructed but NOT init()ed: the precondition is a question about
        // the master's SHAPE (widths, input levels, channel map), and asking it
        // must not need latches, plugs or a graph — that is the whole point of
        // it being an engine helper over the two components.
        // tw303aEnvironment::bufferSize has NO initialiser and no default (see
        // tw/graph/tw303aenv.h): the APP sets it at startup, and a unit test
        // that never does gets whatever was on the stack. twMixer's constructor
        // callocs from it, so it throws on a garbage value. Stated rather than
        // worked around silently — the engine's default is a graph concern, not
        // this phase's (proposal 21 L1a touches playback/plugins/mix).
        env.setBufferSize(4096);

        auto mixer = std::make_shared<twMixer>(env, 2);
        auto root  = std::make_shared<twRewire>(env);
        mixer->setNInputs(2);
        mixer->setChannels(2);
        root->setChannels(2);
        mixer->setInputLevel(0, 0.0);
        mixer->setInputLevel(1, 0.0);

        twlive::twMasterShape sh = twlive::checkMasterShape(mixer.get(), root.get(), 2);
        CHECK(sh.linear(),
              "L1a AC3: a unity twMixer into an identity twRewire is the LINEAR SPLIT");

        // A non-unity input level: the master is no longer a plain sum.
        mixer->setInputLevel(1, -6.0);
        sh = twlive::checkMasterShape(mixer.get(), root.get(), 2);
        CHECK(!sh.linear() && sh.mode == twlive::twMasterMode::Closure,
              "L1a AC3: a non-unity master input level selects CLOSURE mode");
        mixer->setInputLevel(1, 0.0);

        // A non-identity channel map: addition no longer commutes with it.
        root->setChannelMap({ 1, 0 });
        sh = twlive::checkMasterShape(mixer.get(), root.get(), 2);
        CHECK(!sh.linear(), "L1a AC3: a swapped master channel map selects CLOSURE mode");
        root->setChannelMap({ 0, 1 });
        CHECK(twlive::checkMasterShape(mixer.get(), root.get(), 2).linear(),
              "L1a AC3: an EXPLICIT identity map is still the linear split");

        // A width disagreement, and the null cases.
        CHECK(!twlive::checkMasterShape(mixer.get(), root.get(), 6).linear(),
              "L1a AC3: a master narrower than the project selects CLOSURE mode");
        CHECK(!twlive::checkMasterShape(nullptr, root.get(), 2).linear(),
              "L1a AC3: no mixer at all selects CLOSURE mode");
        CHECK(!twlive::checkMasterShape(mixer.get(), nullptr, 2).linear(),
              "L1a AC3: no root rewire selects CLOSURE mode");
    }

    // --- AC1: the speaker's two-lane transitions -----------------------
    {
        printf("\n=== 21 L1a AC1: device x frozen x live ===\n");

        // The capture backend is the only one a headless test may open, and it
        // is chosen at twSpeaker CONSTRUCTION (createAudioBackend reads the
        // environment), so the variable has to be set first.
#if defined(_WIN32)
        _putenv((char *)"SMARAGD_AUDIO_BACKEND=capture");
#else
        setenv("SMARAGD_AUDIO_BACKEND", "capture", 1);
#endif
        auto tone = std::make_shared<ToneComponent>(env);
        tone->init();
        TestPlaybackContext ctx;
        ctx.root = tone;

        auto spk = std::make_shared<twSpeaker>(env);
        spk->init();
        spk->setPlaybackContext(&ctx);
        auto *cap = dynamic_cast<audio::CaptureBackend *>(spk->getBackend());
        CHECK(cap != nullptr, "L1a AC1: the capture backend is selected");
        CHECK(spk->deviceState() == DeviceState::CLOSED && !spk->liveActive(),
              "L1a AC1: a fresh speaker is device CLOSED, live OFF");

        // CLOSED -> OPEN(live). No priming: the callback must run immediately.
        CHECK(spk->openLive((std::uint32_t)env.getSRate(), 1) == 0,
              "L1a AC1: openLive() succeeds");
        CHECK(spk->deviceState() == DeviceState::OPEN && spk->liveActive(),
              "L1a AC1: ...device OPEN, live ON");
        CHECK(spk->getOutputState() == OutputState::STOPPED,
              "L1a AC1: ...and the FROZEN lane is still IDLE");

        bool pumping = false;
        for (int i = 0; i < 200 && !pumping; ++i) {
            if (cap->capturedFrames() > 0) { pumping = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(pumping,
              "L1a AC1: the render callback RUNS with the transport stopped "
              "(the live lane needs no readahead priming)");
        // Let a few blocks accumulate so "the recording was not cleared" has
        // something to be about.
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const std::size_t framesBeforePlay = cap->capturedFrames();
        CHECK(framesBeforePlay > 4096,
              "L1a AC1: ...and keeps producing frames while stopped");

        // PLAY ATTACHES: no second openDevice (which would clearCapture), and a
        // new engine minted under the running callback.
        spk->startOutput();
        bool playing = false;
        for (int i = 0; i < 1500 && !playing; ++i) {
            if (spk->getOutputState() == OutputState::PLAYING) { playing = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(playing, "L1a AC1: PLAY reaches the frozen lane's PLAYING");
        CHECK(spk->deviceState() == DeviceState::OPEN,
              "L1a AC1: ...the device is still the one the live lane opened");
        CHECK(cap->capturedFrames() >= framesBeforePlay,
              "L1a AC1: ...and the recording was NOT cleared, i.e. the device was "
              "NOT re-opened (clearCapture happens at DEVICE open)");
        CHECK(spk->getAudioEngine() != nullptr,
              "L1a AC1: ...an engine was minted under the running callback");

        // STOP keeps the device.
        spk->stopOutput();
        CHECK(spk->getOutputState() == OutputState::STOPPED,
              "L1a AC1: STOP returns the frozen lane to IDLE");
        CHECK(spk->deviceState() == DeviceState::OPEN && spk->liveActive(),
              "L1a AC1: ...and leaves the device OPEN while live is ON");
        const std::size_t afterStop = cap->capturedFrames();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        CHECK(cap->capturedFrames() > afterStop,
              "L1a AC1: ...the callback keeps running after the transport stops");

        // Disarm closes.
        spk->closeLive();
        CHECK(spk->deviceState() == DeviceState::CLOSED && !spk->liveActive(),
              "L1a AC1: closeLive() closes the device");
    }

    // --- AC1b: PLAY WITHOUT LIVE is exactly what it always was ---------
    {
        auto tone = std::make_shared<ToneComponent>(env);
        tone->init();
        TestPlaybackContext ctx;
        ctx.root = tone;

        auto spk = std::make_shared<twSpeaker>(env);
        spk->init();
        spk->setPlaybackContext(&ctx);
        auto *cap = dynamic_cast<audio::CaptureBackend *>(spk->getBackend());

        spk->startOutput();
        bool playing = false;
        for (int i = 0; i < 1500 && !playing; ++i) {
            if (spk->getOutputState() == OutputState::PLAYING) { playing = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(playing, "L1a AC1b: PLAY without a live lane still reaches PLAYING");
        CHECK(spk->deviceState() == DeviceState::OPEN,
              "L1a AC1b: ...and opens the device itself");
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        const std::size_t total1 = cap->capturedFrames();
        CHECK(total1 > 0, "L1a AC1b: ...and records");

        spk->stopOutput();
        CHECK(spk->getOutputState() == OutputState::STOPPED &&
                  spk->deviceState() == DeviceState::CLOSED,
              "L1a AC1b: STOP closes the device when live is OFF (today's behaviour)");
        // >= rather than ==: the callback keeps appending between the read of
        // total1 and the moment stopOutput() actually stops the pump. The claim
        // is that the close does NOT clear, which is what >= says.
        CHECK(cap->capturedFrames() >= total1,
              "L1a AC1b: the recording survives the close (dump-playback-capture "
              "reads it after stopping)");

        // CAPTURED FRAME 0 IS THE FIRST FRAME OF THE DEVICE SESSION (testkit
        // rule 1). startOutput() opens the device and returns BEFORE the
        // readahead has primed, so this is deterministic rather than a race.
        spk->startOutput();
        CHECK(cap->capturedFrames() == 0,
              "L1a AC1b: a second play re-opens the device and frame 0 is the "
              "first frame of THAT session");
        spk->stopOutput();
    }

#ifdef TW_TESTCLAP_PATH
    // --- AC2: the synthetic-plan harness -------------------------------
    //
    // A live plan over the REAL `tw.test.clap.gain` at 0.5 — LINEAR and
    // therefore PARTITION-INVARIANT, which is the ONLY reason a sample-exact
    // claim can be made at all: the pump renders 1024-frame blocks and the
    // freeze path renders 4096-frame chunks, and a stateful plugin would
    // legitimately differ between the two partitions.
    {
        printf("\n=== 21 L1a AC2: the synthetic-plan harness ===\n");

        const int      rate  = (int)env.getSRate();
        const length_t BLOCK = 1024;
        const int      NBLK  = 32;                 // 32768 frames, half a page

        twPluginDescriptor desc;
        desc.format = "clap";
        desc.uid    = "tw.test.clap.gain";
        desc.path   = TW_TESTCLAP_PATH;
        desc.name   = "Smaragd Test Gain";

        auto factory = [desc]() -> std::unique_ptr<twPlugin> {
            return pluginRegistry().instantiate(desc);
        };
        auto setGain = [](twPluginSlotProcessor &p, double g) {
            for (twPlugin *pl : p.plugins()) if (pl) pl->setParam(0, g);
            p.bumpParamEpoch();
        };

        // (1) THE FROZEN REFERENCE, on THIS thread, while the render policy is
        //     still Any — the harness below marks its own thread as the pump.
        auto procRef = std::make_shared<twPluginSlotProcessor>(
            env, factory, twPluginIoLayout{ 2, 2 });
        procRef->setChannelCount(2);
        setGain(*procRef, 0.5);
        auto src = std::make_shared<PosSource>(env, 2);
        src->init();
        auto insert = std::make_shared<twPluginInsert>(env, procRef);
        insert->init();
        insert->setInput(0, src->linkOutput(0));
        std::shared_ptr<twOutputPage> ref = insert->requestPage(
            0, nullptr, 0, (length_t)twOutputPage::FRAME_CAPACITY, rate, nullptr);
        CHECK(ref && ref->channels() == 2 && ref->validFrames >= NBLK * BLOCK,
              "L1a AC2: the frozen reference page renders");

        // (2) THE LIVE PLAN over a SECOND processor with the same plugin at the
        //     same gain, fed by a synthetic input that is a pure function of
        //     position (the same one the reference source produces).
        auto procLive = std::make_shared<twPluginSlotProcessor>(
            env, factory, twPluginIoLayout{ 2, 2 });
        procLive->setChannelCount(2);
        setGain(*procLive, 0.5);
        procLive->setLiveOwned(true);

        auto plan = std::make_shared<twLivePlan>();
        plan->blockFrames = BLOCK;
        plan->sampleRate  = rate;
        // ONE block of lead and a clock stamped per block: the harness IS the
        // device, so it renders exactly the block the RT is about to pull and
        // then idles (review fix 2's pacing, driven synchronously).
        plan->leadFrames  = BLOCK;
        plan->outputTrack = 0;
        plan->transport.playing     = true;
        plan->transport.feedEnabled = true;
        {
            twLiveTrackPlan t;
            t.name     = "live";
            t.channels = 2;
            t.inserts.push_back(procLive);
            t.input = std::make_shared<PosInput>();
            plan->tracks.push_back(std::move(t));
        }
        CHECK(plan->finalize(), "L1a AC2: the plan finalizes");

        twLiveMixRing ring;
        ring.reset(2, (std::uint32_t)BLOCK, 4);
        twEngineClock clock;
        LiveGraphPump pump(ring, clock);
        pump.setPlan(plan);

        std::vector<float> got0(NBLK * BLOCK, 0.0f), got1(NBLK * BLOCK, 0.0f);
        std::uint64_t reposContiguous = 0, reposSeek = 0;
        std::vector<float> seek0(BLOCK, 0.0f);
        offset_t seekAt = 500000;
        bool     seekAligned = false;

        // THE HARNESS THREAD. Synchronous, unpaced: one block, one drain. It
        // marks itself the live pump (renderOneBlock does), and that marker is
        // sticky — which is why it is a thread of its own.
        std::thread harness([&] {
            for (int b = 0; b < NBLK; ++b) {
                // The device consuming: stamp what it will pull NEXT (and the
                // frame it is delivering, one buffer behind), then render.
                clock.stamp((std::int64_t)b * BLOCK - (std::int64_t)BLOCK,
                            (std::int64_t)b * BLOCK, 0);
                if (b == 1) pump.resetStats();   // drop the ADOPTION reposition
                if (!pump.renderOneBlock()) break;
                twLiveRingEntry e;
                if (!ring.peek(e)) break;
                std::copy(e.channel(0), e.channel(0) + BLOCK, got0.begin() + b * BLOCK);
                std::copy(e.channel(1), e.channel(1) + BLOCK, got1.begin() + b * BLOCK);
                ring.pop();
            }
            reposContiguous = pump.repositions();

            // A SEEK MID-RUN: one jump, then the device consuming from there.
            pump.resetStats();
            clock.stamp((std::int64_t)seekAt - (std::int64_t)BLOCK, seekAt, 0);
            if (pump.renderOneBlock()) {
                twLiveRingEntry e;
                if (ring.peek(e)) {
                    seekAligned = (e.startPos == (std::int64_t)seekAt);
                    std::copy(e.channel(0), e.channel(0) + BLOCK, seek0.begin());
                    ring.pop();
                }
            }
            // Two more blocks at the new position must NOT reposition again.
            for (int b = 1; b <= 2; ++b) {
                clock.stamp(seekAt + (std::int64_t)b * BLOCK - (std::int64_t)BLOCK,
                            seekAt + (std::int64_t)b * BLOCK, 0);
                if (!pump.renderOneBlock()) break;
                twLiveRingEntry e;
                if (ring.peek(e)) ring.pop();
            }
            reposSeek = pump.repositions();
        });
        harness.join();

        // (3) SAMPLE-EXACT over the contiguous run.
        std::size_t diffs = 0;
        std::size_t firstDiff = (std::size_t)-1;
        for (std::size_t i = 0; i < (std::size_t)(NBLK * BLOCK); ++i) {
            if (got0[i] != ref->channelPtr(0)[i] || got1[i] != ref->channelPtr(1)[i]) {
                if (!diffs) firstDiff = i;
                ++diffs;
            }
        }
        if (diffs)
            printf("     first difference at frame %llu: live (%g, %g) vs frozen (%g, %g)\n",
                   (unsigned long long)firstDiff, (double)got0[firstDiff],
                   (double)got1[firstDiff], (double)ref->channelPtr(0)[firstDiff],
                   (double)ref->channelPtr(1)[firstDiff]);
        CHECK(diffs == 0,
              "L1a AC2: the live blocks equal the frozen render of the same "
              "material through the same insert, SAMPLE FOR SAMPLE");
        CHECK(got0[100] != 0.0f, "L1a AC2: ...and the comparison is not over silence");

        CHECK(reposContiguous == 0,
              "L1a AC2: a contiguous run causes NO repositions after the first block");
        CHECK(reposSeek == 1,
              "L1a AC2: a seek mid-run causes EXACTLY ONE reposition");
        CHECK(seekAligned, "L1a AC2: ...and the next block is stamped at the seek target");
        bool realigned = true;
        for (std::size_t i = 0; i < 64; ++i)
            if (seek0[i] != PosSource::value(0, (offset_t)seekAt + (offset_t)i) * 0.5f)
                realigned = false;
        CHECK(realigned,
              "L1a AC2: ...and the output re-aligns to the material at the new position");
    }

    // --- AC2 (stopped): the feed gate, the live source, the automation hold ---
    {
        printf("\n=== 21 L1a AC2: STOPPED — feed masked, live heard, automation held ===\n");

        const int      rate  = (int)env.getSRate();
        const length_t BLOCK = 1024;

        twPluginDescriptor sineDesc;
        sineDesc.format = "clap";
        sineDesc.uid    = "tw.test.clap.sine";
        sineDesc.path   = TW_TESTCLAP_PATH;
        auto sineFactory = [sineDesc]() -> std::unique_ptr<twPlugin> {
            return pluginRegistry().instantiate(sineDesc);
        };

        auto feed = std::make_shared<OneNoteSource>(20, 200000, 60, 1);
        auto live = std::make_shared<OneNoteSource>(10, 200000, 67, 2);

        // `stopped` runs ONE block of a stopped plan and returns its peak.
        auto stoppedPeak = [&](bool feedEnabled, bool withLive) -> float {
            auto proc = std::make_shared<twPluginSlotProcessor>(
                env, sineFactory, twPluginIoLayout{ 0, 2 });
            proc->setChannelCount(2);
            proc->setEventSource(feed);
            proc->setLiveOwned(true);
            if (withLive) proc->setLiveEventSource(live);

            auto plan = std::make_shared<twLivePlan>();
            plan->blockFrames  = BLOCK;
            plan->sampleRate   = rate;
            plan->outputTrack  = 0;
            plan->stoppedAnchor = 0;
            plan->transport.playing          = false;
            plan->transport.feedEnabled      = feedEnabled;
            plan->transport.holdAutomationAt = 0;
            twLiveTrackPlan t;
            t.name     = "inst";
            t.channels = 2;
            t.inserts.push_back(proc);
            plan->tracks.push_back(std::move(t));
            plan->finalize();

            twLiveMixRing ring;
            ring.reset(2, (std::uint32_t)BLOCK, 4);
            twEngineClock clock;
            clock.invalidate();          // the transport is stopped: no anchor
            LiveGraphPump pump(ring, clock);
            pump.setPlan(plan);

            float peak = 0.0f;
            std::thread th([&] {
                pump.renderOneBlock();
                twLiveRingEntry e;
                if (ring.peek(e)) {
                    for (std::uint32_t i = 0; i < e.frames; ++i)
                        peak = std::max(peak, std::fabs(e.channel(0)[i]));
                    ring.pop();
                }
            });
            th.join();
            return peak;
        };

        const float withLiveOnly = stoppedPeak(/*feedEnabled=*/false, /*withLive=*/true);
        const float neither      = stoppedPeak(/*feedEnabled=*/false, /*withLive=*/false);
        const float feedOnly     = stoppedPeak(/*feedEnabled=*/true,  /*withLive=*/false);

        CHECK(neither < 1e-6f,
              "L1a AC2 stopped: with feedEnabled=false NO sequenced material "
              "reaches the processor — the block is EXACTLY silent");
        CHECK(withLiveOnly > 0.1f,
              "L1a AC2 stopped: ...while an injected LIVE event does sound");
        CHECK(feedOnly > 0.1f,
              "L1a AC2 stopped: ...and the masked feed is a real source (it sounds "
              "when feedEnabled is true), so the mask has teeth");

        // THE AUTOMATION HOLD. A ramp 0 -> 1 over [0, 100000) on the gain
        // plugin's param 0, a constant 1.0 input, and holdAutomationAt = 50000:
        // the whole block must read the value UNDER THE PLAYHEAD, not sweep.
        {
            twPluginDescriptor desc;
            desc.format = "clap";
            desc.uid    = "tw.test.clap.gain";
            desc.path   = TW_TESTCLAP_PATH;
            auto proc = std::make_shared<twPluginSlotProcessor>(
                env, [desc] { return pluginRegistry().instantiate(desc); },
                twPluginIoLayout{ 2, 2 });
            proc->setChannelCount(2);
            proc->setLiveOwned(true);

            std::vector<twCurvePoint> pts;
            pts.push_back(twCurvePoint{ 0,      0.0, twCurveShape::Linear, 0.0 });
            pts.push_back(twCurvePoint{ 100000, 1.0, twCurveShape::Linear, 0.0 });
            auto curve = std::make_shared<const twAutomationCurve>(pts, 0.0);
            std::map<std::uint32_t, std::shared_ptr<const twAutomationCurve> > curves;
            curves[0] = curve;
            proc->setParamCurves(curves);
            CHECK(std::fabs(curve->valueAt(50000) - 0.5) < 1e-9,
                  "L1a AC2 hold: the curve really reads 0.5 at the held position");

            auto plan = std::make_shared<twLivePlan>();
            plan->blockFrames   = BLOCK;
            plan->sampleRate    = rate;
            plan->outputTrack   = 0;
            plan->stoppedAnchor = 0;
            plan->transport.playing          = false;
            plan->transport.feedEnabled      = false;
            plan->transport.holdAutomationAt = 50000;
            twLiveTrackPlan t;
            t.name     = "fx";
            t.channels = 2;
            t.inserts.push_back(proc);
            t.input = std::make_shared<ConstInput>(1.0f);
            plan->tracks.push_back(std::move(t));
            plan->finalize();

            twLiveMixRing ring;
            ring.reset(2, (std::uint32_t)BLOCK, 4);
            twEngineClock clock;
            clock.invalidate();
            LiveGraphPump pump(ring, clock);
            pump.setPlan(plan);

            float lo = 1e9f, hi = -1e9f;
            std::thread th([&] {
                pump.renderOneBlock();
                twLiveRingEntry e;
                if (ring.peek(e)) {
                    for (std::uint32_t i = 0; i < e.frames; ++i) {
                        lo = std::min(lo, e.channel(0)[i]);
                        hi = std::max(hi, e.channel(0)[i]);
                    }
                    ring.pop();
                }
            });
            th.join();
            CHECK(std::fabs(hi - lo) < 1e-6f,
                  "L1a AC2 hold: an automated parameter is CONSTANT across the "
                  "block while the transport is stopped");
            CHECK(std::fabs(lo - 0.5f) < 1e-5f,
                  "L1a AC2 hold: ...and constant at holdAutomationAt's value, not "
                  "at the pump's virtual position");
        }
    }

    // --- AC4: the live-ownership guard ---------------------------------
    {
        printf("\n=== 21 L1a AC4: liveOwnedRefusals ===\n");

        twPluginDescriptor desc;
        desc.format = "clap";
        desc.uid    = "tw.test.clap.gain";
        desc.path   = TW_TESTCLAP_PATH;
        auto proc = std::make_shared<twPluginSlotProcessor>(
            env, [desc] { return pluginRegistry().instantiate(desc); },
            twPluginIoLayout{ 2, 2 });
        proc->setChannelCount(2);

        constexpr length_t N = 256;
        std::vector<float> inA(N, 0.25f), inB(N, 0.25f), outA(N, -1.0f), outB(N, -1.0f);
        const float *ins[2]  = { inA.data(), inB.data() };
        float       *outs[2] = { outA.data(), outB.data() };

        twPluginSlotProcessor::resetLiveOwnedCounters();
        const std::uint64_t before = twPluginSlotProcessor::liveOwnedRefusals();

        // A freeze-path render from a thread that is NOT the pump.
        proc->setLiveOwned(true);
        std::thread freezer([&] {
            proc->render(ins, outs, N, 0, /*positional=*/true, (int)env.getSRate());
        });
        freezer.join();
        CHECK(outA[0] == 0.0f && outB[0] == 0.0f,
              "L1a AC4: a render on a LIVE-OWNED processor from a non-pump thread "
              "answers SILENCE");
        CHECK(twPluginSlotProcessor::liveOwnedRefusals() == before + 1,
              "L1a AC4: ...and is counted exactly once");

        // Handed back, it renders again.
        proc->setLiveOwned(false);
        std::fill(outA.begin(), outA.end(), -1.0f);
        std::thread again([&] {
            proc->render(ins, outs, N, 0, /*positional=*/true, (int)env.getSRate());
        });
        again.join();
        CHECK(std::fabs(outA[0] - 0.25f) < 1e-6f,
              "L1a AC4: after setLiveOwned(false) the same call renders normally");
        CHECK(twPluginSlotProcessor::liveOwnedRefusals() == before + 1,
              "L1a AC4: ...and nothing further is counted");
    }
#else
    printf("\n  note 21 L1a AC2/AC4 SKIPPED (built without the CLAP fixture)\n");
#endif

    // ==================================================================
    // PROPOSAL 21 L1a — ORCHESTRATOR REVIEW FIXES.
    //
    // T1 the ring consumed as a STREAM by a VARIABLE-block RT (fix 1);
    // T2 the pump PACED ON THE CLOCK, threaded (fix 2);
    // T3 both of them end to end through the real render callback;
    // T4 the live lane REFUSES a device whose rate is not the project's (fix 3).
    // ==================================================================

    // --- T1: the stream consumer, variable blocks ----------------------
    {
        printf("\n=== 21 L1a T1: the ring is a STREAM (variable RT blocks) ===\n");

        constexpr std::uint32_t ENTRY = 1024;
        constexpr std::uint32_t DEPTH = 8;

        twLiveMixRing   ring;
        twLiveMixReader reader;
        twLiveMixState  fade;                 // fadeFrames 0 == a plain sum
        ring.reset(1, ENTRY, DEPTH);

        std::int64_t producePos = 0;
        auto produce = [&](std::int64_t upto, std::uint64_t flipEpoch) {
            while (producePos < upto) {
                float *w = ring.beginWrite();
                if (!w) break;
                for (std::uint32_t i = 0; i < ENTRY; ++i)
                    w[i] = rampAt(producePos + (std::int64_t)i);
                ring.commit(producePos, ENTRY, flipEpoch, 0, true);
                producePos += (std::int64_t)ENTRY;
            }
        };

        // THE IRREGULAR GRID. Nothing here is a multiple of the pump's block,
        // two of them are smaller than one entry and one spans two — which is
        // what a WASAPI callback asking for `bufferFrames - padding` looks like
        // (design F6), and what the old exact-equality gate could not serve at
        // all.
        const std::size_t BLOCKS[] = { 480, 1056, 33, 2048, 1024, 7 };
        std::vector<float> out(4096, 0.0f);
        float *outs[1] = { out.data() };

        std::int64_t want   = 0;
        std::size_t  bad    = 0, produced = 0;
        std::int64_t firstBad = -1;
        std::uint32_t gaps  = 0;

        twLiveMixGate g;
        // The RT is the POSITION AUTHORITY here (haveRoot): the seeks below are
        // measured against wantPos. Without a root the stream is consumed
        // sequentially and a "seek" is not a thing the consumer can see (that
        // path is asserted separately, right after this block). rootEpoch 0 and
        // flipEpoch 0 entries ⇒ every entry passes the epoch gate.
        g.haveRoot = true;

        for (int round = 0; round < 40; ++round) {
            const std::size_t n = BLOCKS[round % 6];
            produce(want + (std::int64_t)n + (std::int64_t)ENTRY, 0);
            std::fill(out.begin(), out.begin() + (std::ptrdiff_t)n, 0.0f);
            g.wantPos = want;
            twLiveStreamStats st;
            twlive::mixStream(outs, 1, n, want, g, ring, reader, fade, st);
            gaps += st.notYet + st.starved;
            for (std::size_t i = 0; i < n; ++i) {
                const float expect = rampAt(want + (std::int64_t)i);
                if (out[i] != expect) { if (firstBad < 0) firstBad = want + (std::int64_t)i; ++bad; }
                ++produced;
            }
            want += (std::int64_t)n;
        }
        if (bad)
            printf("     first wrong frame at %lld\n", (long long)firstBad);
        CHECK(bad == 0,
              "L1a T1: a VARIABLE-block RT reads the pump's fixed blocks "
              "SAMPLE-FOR-SAMPLE across every partition");
        CHECK(produced > 20000, "L1a T1: ...over enough frames to mean it");
        CHECK(gaps == 0, "L1a T1: ...with no gap, because the pump stayed ahead");
        CHECK(ring.mismatches() == 0,
              "L1a T1: the exact-position MISMATCH counter never fires on a "
              "stream — that was the defect");

        // A SEEK FORWARD past what the ring holds: the entries behind are
        // DROPPED and counted, and the audio at the new position is still exact.
        {
            const std::int64_t target = producePos + 4 * (std::int64_t)ENTRY;
            produce(target + 2 * (std::int64_t)ENTRY, 0);
            std::fill(out.begin(), out.begin() + 512, 0.0f);
            g.wantPos = target;
            twLiveStreamStats st;
            twlive::mixStream(outs, 1, 512, target, g, ring, reader, fade, st);
            bool ok = true;
            for (std::size_t i = 0; i < 512; ++i)
                if (out[i] != rampAt(target + (std::int64_t)i)) ok = false;
            CHECK(st.dropped >= 1,
                  "L1a T1 seek forward: the entries the RT passed are DROPPED and counted");
            CHECK(ok, "L1a T1 seek forward: ...and the new position reads exactly");
        }

        // A SEEK BACK with no pump reaction: the head is the FUTURE, so the RT
        // gets silence and counts notYet — and CRUCIALLY does not pop it, or
        // the audio the next callback needs would be gone.
        {
            const std::uint32_t before = ring.pending();
            std::fill(out.begin(), out.begin() + 256, 0.0f);
            const std::int64_t back = 0;
            g.wantPos = back;
            twLiveStreamStats st;
            twlive::mixStream(outs, 1, 256, back, g, ring, reader, fade, st);
            bool silent = true;
            for (std::size_t i = 0; i < 256; ++i) if (out[i] != 0.0f) silent = false;
            CHECK(st.notYet >= 1 && silent,
                  "L1a T1 seek back: the head is the FUTURE — silence, counted");
            CHECK(ring.pending() == before,
                  "L1a T1 seek back: ...and NOTHING is popped (the next callback needs it)");
        }

        // NO ROOT (STOPPED): the stream is its own authority and is consumed
        // SEQUENTIALLY -- the entry in hand IS the want. So a new RUN (a
        // reposition while stopped) plays from its first frame regardless of
        // where the abandoned run stood: no gap, no skipped head.
        {
            twLiveMixGate gs; gs.haveRoot = false;
            // Drain whatever the seek-back left queued (all still the old run).
            {
                std::vector<float> junk(65536, 0.0f);
                float *j[1] = { junk.data() };
                twLiveStreamStats st;
                twlive::mixStream(j, 1, 65536, 0, gs, ring, reader, fade, st);
            }
            ring.setRun(ring.currentRun() + 1);
            const std::int64_t newPos = 7;   // arbitrary, far from the old run
            producePos = newPos;
            produce(newPos + 3 * (std::int64_t)ENTRY, 0);
            std::fill(out.begin(), out.begin() + 300, 0.0f);
            twLiveStreamStats st;
            twlive::mixStream(outs, 1, 300, /*wantPos ignored*/ 123456, gs, ring, reader, fade, st);
            bool ok = true;
            for (std::size_t i = 0; i < 300; ++i)
                if (out[i] != rampAt(newPos + (std::int64_t)i)) ok = false;
            CHECK(ok && st.framesSummed == 300 && st.notYet == 0 && st.starved == 0,
                  "L1a T1 no-root: a new run is consumed sequentially from its frame 0");
        }

        // THE EPOCH GATE, PER ENTRY, ACROSS A PARTIAL CONSUMPTION. The verdict
        // comes from the entry's epochs against the ROOT PAGE IN HAND, so a
        // flip that lands mid-entry takes effect mid-entry — which is the whole
        // point of gating on the served page rather than on a block count.
        {
            twLiveMixRing   r2;
            twLiveMixReader rd2;
            twLiveMixState  f2;
            r2.reset(1, ENTRY, 4);
            float *w = r2.beginWrite();
            for (std::uint32_t i = 0; i < ENTRY; ++i) w[i] = 1.0f;
            r2.commit(0, ENTRY, /*flipEpoch=*/100, 0, true);

            std::vector<float> o(ENTRY, 0.0f);
            float *os[1] = { o.data() };
            twLiveMixGate gg; gg.haveRoot = true; gg.wantPos = 0;

            gg.rootEpoch = 99;
            twLiveStreamStats s1;
            twlive::mixStream(os, 1, 33, 0, gg, r2, rd2, f2, s1);
            CHECK(s1.gated == 1 && o[0] == 0.0f,
                  "L1a T1 epoch: a partial consumption before the flip is GATED");
            CHECK(rd2.cursor == 33,
                  "L1a T1 epoch: ...and still advances the cursor (the frames are spent)");

            gg.rootEpoch = 100;
            gg.wantPos   = 33;
            twLiveStreamStats s2;
            twlive::mixStream(os, 1, 64, 33, gg, r2, rd2, f2, s2);
            CHECK(s2.summed == 1 && o[0] == 1.0f,
                  "L1a T1 epoch: the REST of the same entry is summed once the "
                  "re-summed root page lands");
        }
    }

    // --- T2: the pump paced on the clock, threaded ---------------------
    {
        printf("\n=== 21 L1a T2: the pump PACES on the clock (threaded) ===\n");

        const length_t BLOCK = 1024;
        const length_t LEAD  = 4 * BLOCK;
        const std::uint32_t DEPTH = 6;          // ceil(LEAD/BLOCK) + 2

        auto plan = std::make_shared<twLivePlan>();
        plan->blockFrames = BLOCK;
        plan->sampleRate  = (int)env.getSRate();
        plan->leadFrames  = LEAD;
        plan->outputTrack = 0;
        plan->transport.playing = true;
        {
            twLiveTrackPlan t;
            t.name     = "ramp";
            t.channels = 1;
            t.input    = std::make_shared<RampInput>();
            plan->tracks.push_back(std::move(t));
        }
        CHECK(plan->finalize(), "L1a T2: the plan finalizes");
        CHECK(plan->requiredRingDepth() == DEPTH,
              "L1a T2: requiredRingDepth is ceil(lead/block) + 2");

        twLiveMixRing   ring;
        twLiveMixReader reader;
        twLiveMixState  fade;
        ring.reset(1, (std::uint32_t)BLOCK, DEPTH);
        twEngineClock clock;
        LiveGraphPump pump(ring, clock);
        pump.setPlan(plan);
        pump.start();

        // The pretend RT: a variable block every ~1 ms, stamping what it will
        // pull NEXT from its own counter. It is not the audio thread and does
        // not pretend to be one; what it reproduces is the SHAPE the pump has
        // to survive — an irregular grid and a clock it does not control.
        const std::size_t BLOCKS[] = { 480, 1056, 33, 2048, 1024, 7 };
        std::vector<float> out(4096, 0.0f);
        float *outs[1] = { out.data() };
        twLiveMixGate g; g.haveRoot = false;

        std::int64_t want = 0;
        std::size_t  bad = 0, summedFrames = 0, silentFrames = 0;
        std::uint32_t maxPending = 0;

        // `realtime` paces the pretend RT at the rate a device would: a block of
        // n frames takes n/48 ms at 48 kHz. The fast 1 ms tick is a DELIBERATE
        // over-drive for the steady-state run — it consumes ~10x real time and
        // is the harshest pacing pressure the pump can be put under — but it is
        // the wrong instrument for a SEEK, where the pump has to shed the
        // abandoned run and refill a whole lead before the RT arrives. At 10x
        // real time it gets a tenth of the budget a real device would give it
        // and can legitimately need a second reposition; that would be
        // measuring the test's clock, not the pump.
        auto runBlocks = [&](int count, bool realtime = false) {
            for (int i = 0; i < count; ++i) {
                const std::size_t n = BLOCKS[i % 6];
                clock.stamp(want - (std::int64_t)BLOCK, want, 0);
                std::this_thread::sleep_for(std::chrono::milliseconds(
                    realtime ? (long long)std::max<std::size_t>(1, n / 48) : 1LL));
                maxPending = std::max(maxPending, ring.pending());
                std::fill(out.begin(), out.begin() + (std::ptrdiff_t)n, 0.0f);
                g.wantPos = want;
                twLiveStreamStats st;
                twlive::mixStream(outs, 1, n, want, g, ring, reader, fade, st);
                for (std::size_t k = 0; k < n; ++k) {
                    const float expect = rampAt(want + (std::int64_t)k);
                    if (out[k] == 0.0f)        ++silentFrames;
                    else if (out[k] != expect) ++bad;
                    else                       ++summedFrames;
                }
                want += (std::int64_t)n;
            }
        };

        // The RT STANDING STILL: stamping where it is and consuming nothing. It
        // is how every reposition below is COUNTED, and the reason is not
        // convenience. A seek makes the pump shed the run it had queued and
        // re-cover a whole lead; if the RT keeps advancing through that
        // refill it can outrun the pump once more and earn a second, perfectly
        // correct reposition — so a window that spans the refill measures the
        // refill, not the seek. With the clock static the pump repositions
        // once, renders up to its lead and then idles, which makes "exactly
        // one" deterministic. Continuity after the seek is then asserted
        // separately, over a moving window, which is the other half of the
        // claim and the half a still window cannot make.
        auto holdStill = [&](int ticks) {
            for (int i = 0; i < ticks; ++i) {
                clock.stamp(want - (std::int64_t)BLOCK, want, 0);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        };

        runBlocks(40);                 // warm up: let the pump reach its lead
        pump.resetStats();
        bad = summedFrames = silentFrames = 0;
        maxPending = 0;
        runBlocks(500);

        const std::uint64_t reposSteady = pump.repositions();
        printf("     steady: %llu blocks, %llu summed frames, %llu silent, "
               "maxPending=%u, repositions=%llu\n",
               500ULL, (unsigned long long)summedFrames,
               (unsigned long long)silentFrames, (unsigned)maxPending,
               (unsigned long long)reposSteady);

        CHECK(bad == 0,
              "L1a T2: every frame the stream delivered is the RIGHT frame "
              "(contiguous positions, no duplication)");
        CHECK(reposSteady == 0,
              "L1a T2: a steady run causes NO repositions — the old fill-until-full "
              "pump repositioned on every clock stamp");
        CHECK(maxPending <= DEPTH - 1,
              "L1a T2: the pump never runs further ahead than its lead "
              "(occupancy <= ceil(lead/block) + 1)");
        // Timing-dependent, so it is REPORTED and bounded loosely rather than
        // asserted tightly: this is a 1 ms tick against a 2 ms pump nap on a
        // machine that is also running a test suite.
        CHECK(silentFrames * 20 < summedFrames,
              "L1a T2: starvation stays under 5% of frames on an idle box "
              "(reported above; a loose bound on purpose)");

        // ONE SEEK FORWARD, then ONE SEEK BACK. Each is exactly one reposition.
        pump.resetStats();
        want += 2000000;                       // far past the covered range
        holdStill(20);
        CHECK(pump.repositions() == 1, "L1a T2: a seek FORWARD is exactly one reposition");

        pump.resetStats();
        bad = summedFrames = silentFrames = 0;
        runBlocks(30, /*realtime=*/true);
        CHECK(pump.repositions() == 0 && bad == 0 && summedFrames > 0,
              "L1a T2: ...and continuity resumes at the new position");

        pump.resetStats();
        want = 4096;                           // back, far behind the lead
        holdStill(20);
        CHECK(pump.repositions() == 1, "L1a T2: a seek BACK is exactly one reposition");

        pump.resetStats();
        bad = summedFrames = silentFrames = 0;
        runBlocks(30, /*realtime=*/true);
        CHECK(pump.repositions() == 0 && bad == 0 && summedFrames > 0,
              "L1a T2: ...and continuity resumes there too");

        // The explicit request the app will use (design D2), independent of any
        // drift the clock happens to show.
        //
        // Measured with the RT STANDING STILL — stamping the same nextFrame and
        // consuming nothing. That is not a convenience: it removes the refill
        // transient from the window, so what is asserted is the REQUEST and not
        // how fast the pump can re-cover its lead afterwards. With the clock
        // static the pump repositions once, renders up to its lead, and then
        // idles, so "exactly one" is deterministic rather than a race.
        pump.resetStats();
        holdStill(20);
        CHECK(pump.repositions() == 0,
              "L1a T2 control: a clock that does not move causes NO reposition");

        pump.resetStats();
        pump.requestReposition();
        holdStill(20);
        CHECK(pump.repositions() == 1,
              "L1a T2: requestReposition() forces exactly one, with no clock jump");

        pump.stop();
        CHECK(!pump.running(), "L1a T2: the pump thread joins");
    }

    // --- T3: end to end, through the REAL render callback --------------
    {
        printf("\n=== 21 L1a T3: the live lane through the real callback ===\n");

#if defined(_WIN32)
        _putenv((char *)"SMARAGD_AUDIO_BACKEND=capture");
#else
        setenv("SMARAGD_AUDIO_BACKEND", "capture", 1);
#endif
        auto tone = std::make_shared<ToneComponent>(env);
        tone->init();
        TestPlaybackContext ctx;
        ctx.root = tone;

        auto spk = std::make_shared<twSpeaker>(env);
        spk->init();
        spk->setPlaybackContext(&ctx);
        auto *cap = dynamic_cast<audio::CaptureBackend *>(spk->getBackend());

        const std::uint32_t rate = (std::uint32_t)env.getSRate();
        CHECK(spk->openLive(rate, 1) == 0, "L1a T3: openLive() succeeds at the project rate");
        const length_t BLOCK = (length_t)spk->getBackend()->getConfig().bufferFrames;

        // STOPPED: the pump runs its virtual counter from 0 and the RT consumes
        // the stream sequentially. No crossfade, so the assertion is about the
        // samples and not about a ramp.
        spk->setLiveCrossfadeMs(0.0);

        auto plan = std::make_shared<twLivePlan>();
        plan->blockFrames   = BLOCK;
        plan->sampleRate    = (int)rate;
        plan->outputTrack   = 0;
        plan->stoppedAnchor = 0;
        plan->transport.playing     = false;
        plan->transport.feedEnabled = false;
        {
            twLiveTrackPlan t;
            t.name     = "ramp";
            t.channels = 1;
            t.input    = std::make_shared<RampInput>();
            plan->tracks.push_back(std::move(t));
        }
        plan->finalize();

        LiveGraphPump pump(spk->liveRing(), spk->engineClock());
        pump.setPlan(plan);
        pump.start();

        std::this_thread::sleep_for(std::chrono::milliseconds(400));

        audio::CaptureBackend::CaptureBuffer rec = cap->capturedAudio();
        const std::uint32_t nch = rec.channels ? rec.channels : 1;
        CHECK(rec.frames() > 4096,
              "L1a T3: the callback recorded while the transport was STOPPED");

        // The recording is the ramp: find where it starts, then require a long
        // CONTIGUOUS run of it. A gap would show as a zero (the ramp never is).
        std::size_t start = 0;
        while (start < rec.frames() && rec.samples[start * nch] == 0.0f) ++start;
        std::size_t run = 0, mismatched = 0;
        for (std::size_t i = start; i < rec.frames(); ++i) {
            const float got = rec.samples[i * nch];
            if (got == 0.0f) break;                 // a gap ends the run
            if (got != rampAt((std::int64_t)(i - start))) { ++mismatched; break; }
            ++run;
        }
        printf("     stopped: %llu frames recorded, ramp starts at %llu, "
               "contiguous run %llu\n",
               (unsigned long long)rec.frames(), (unsigned long long)start,
               (unsigned long long)run);
        CHECK(mismatched == 0 && run >= 4096,
              "L1a T3: the recording IS the ramp, contiguous, from the first "
              "block the pump produced");
        if (nch >= 2)
            CHECK(rec.samples[(start + 10) * nch + 1] == rec.samples[(start + 10) * nch],
                  "L1a T3: ...on both device channels (a mono project is L = R = ch0)");

        // NOW ATTACH THE FROZEN LANE. The plan goes to PLAYING so the pump
        // follows the engine clock (this is what L1b's rebuild does), and the
        // transport change is one explicit reposition.
        const std::size_t framesBeforePlay = cap->capturedFrames();
        spk->startOutput();
        bool playing = false;
        for (int i = 0; i < 1500 && !playing; ++i) {
            if (spk->getOutputState() == OutputState::PLAYING) { playing = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(playing, "L1a T3: PLAY attaches to the device the live lane opened");
        CHECK(cap->capturedFrames() >= framesBeforePlay,
              "L1a T3: ...with no re-open, so the recording has no hole");

        auto plan2 = std::make_shared<twLivePlan>(*plan);
        plan2->transport.playing = true;
        plan2->leadFrames        = 2 * BLOCK;
        plan2->finalize();
        pump.requestReposition();
        pump.setPlan(plan2);

        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        rec = cap->capturedAudio();
        pump.stop();

        // The tone is a constant 0.25 and the ramp never leaves [0.25, 0.75],
        // so a SUMMED frame is >= 0.5 and either lane alone is < 0.5 + eps.
        // Counting them is what shows the two lanes are both in the output.
        std::size_t summed = 0, holes = 0;
        for (std::size_t i = framesBeforePlay; i < rec.frames(); ++i) {
            const float v = rec.samples[i * nch];
            if (v == 0.0f) ++holes;
            else if (v >= 0.4999f) ++summed;
        }
        printf("     playing: %llu frames after the attach, %llu carry BOTH lanes, "
               "%llu holes\n",
               (unsigned long long)(rec.frames() - framesBeforePlay),
               (unsigned long long)summed, (unsigned long long)holes);
        CHECK(summed > 4096,
              "L1a T3: after the attach the output carries the frozen material "
              "SUMMED with the ring (tone 0.25 + ramp >= 0.25)");
        spk->stopOutput();
        spk->closeLive();
    }

    // --- T4: the rate scope ---------------------------------------------
    {
        printf("\n=== 21 L1a T4: the live lane refuses a rate mismatch ===\n");

        auto tone = std::make_shared<ToneComponent>(env);
        tone->init();
        TestPlaybackContext ctx;
        ctx.root = tone;

        auto spk = std::make_shared<twSpeaker>(env);
        spk->init();
        spk->setPlaybackContext(&ctx);
        const std::uint32_t rate = (std::uint32_t)env.getSRate();

        // (a) THE DEVICE IS OURS. Both synthetic backends ADOPT the rate they
        //     are opened with, so the only way to reach a closed-device
        //     mismatch is to ask for one they cannot adopt: rate 0 keeps their
        //     default, which is then not the rate we asked for.
        CHECK(spk->openLive(0, 1) == -1,
              "L1a T4: openLive() at a rate the device will not take is REFUSED");
        CHECK(spk->liveRateRefusals() == 1, "L1a T4: ...and counted");
        CHECK(!spk->liveActive() && spk->deviceState() == DeviceState::CLOSED,
              "L1a T4: ...and the device WE opened is closed again");

        // (b) THE DEVICE IS THE FROZEN LANE'S. A refused arm must leave it
        //     completely alone — the arrangement is playing through it.
        spk->startOutput();
        bool playing = false;
        for (int i = 0; i < 1500 && !playing; ++i) {
            if (spk->getOutputState() == OutputState::PLAYING) { playing = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(playing, "L1a T4: the frozen lane is playing");
        CHECK(spk->openLive(rate + 1000, 1) == -1,
              "L1a T4: arming at the wrong rate on a busy device is REFUSED");
        CHECK(spk->liveRateRefusals() == 2, "L1a T4: ...and counted");
        CHECK(!spk->liveActive() && spk->deviceState() == DeviceState::OPEN &&
                  spk->getOutputState() == OutputState::PLAYING,
              "L1a T4: ...and the frozen lane keeps its device, still playing");

        // ...and the matching rate is accepted on the very same device.
        CHECK(spk->openLive(rate, 1) == 0,
              "L1a T4: the project's own rate is accepted on the open device");
        CHECK(spk->liveActive() && spk->liveRateRefusals() == 2,
              "L1a T4: ...with no further refusal");
        spk->stopOutput();
        spk->closeLive();
        CHECK(spk->deviceState() == DeviceState::CLOSED, "L1a T4: everything closes");
    }

    printf(failures ? "\n%d FAILURE(S)\n" : "\nall playback tests passed\n",
           failures);
    return failures ? 1 : 0;
}
