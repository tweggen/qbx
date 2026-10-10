// tw/render module test: RenderSession end-to-end against a scripted
// component — absolute-position ranges (a marked range does NOT start at
// project 0), page-boundary continuity, and file completeness. Normative
// background: render/CONTRACT.md, FREEZE_PROTOCOL.md, POSITION_DOMAINS.md
// rule 6.
#include "tw/render/render_session.h"
#include "tw/graph/twcomponent.h"
#include "tw/graph/tw303aenv.h"

#include <sndfile.h>

#include <atomic>
#include <string>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <fstream>
#include <memory>
#include <thread>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (cond) { printf("ok   %s\n", msg); }                             \
        else      { printf("FAIL %s\n", msg); ++failures; }                 \
    } while (0)

static float val(long long p) { return (float)((p % 977) + 1) / 1000.0f; }

// Scripted source emitting val(absolutePosition) — see mix_test.cc.
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

// A SIX-channel source whose channel c is the constant 0.6 - 0.1*c (proposal
// 36 B5). Constants rather than a ramp on purpose: a per-channel level is
// exactly what a render must not smear, and a wrong channel is then off by a
// whole 0.1 instead of by a phase.
class WideRampComponent : public twComponent {
public:
    static constexpr idx_t kChannels = 6;
    explicit WideRampComponent(tw303aEnvironment &e) : twComponent(e) {}
    bool isSeekable() const override { return true; }
    int seekTo(offset_t) override { return 0; }
    void reset() override {}
    idx_t getOutputChannels() const override { return kChannels; }
    length_t renderPageWide(twOutputPage &page, length_t frames,
                            const sample_t *, length_t) override {
        length_t n = frames;
        if (n > (length_t)page.channelFrames()) n = (length_t)page.channelFrames();
        for (idx_t c = 0; c < (idx_t)page.channels(); ++c) {
            sample_t *dst = page.channelPtr(c);
            const float v = 0.6f - 0.1f * (float)c;
            for (length_t i = 0; i < n; ++i) dst[i] = v;
        }
        return n;
    }
    // The narrow degradation (proposal 36 §7 trap 18).
    length_t renderFrames(sample_t *out, length_t n, const sample_t *,
                          length_t, idx_t) override {
        for (length_t i = 0; i < n; ++i) out[i] = 0.6f;
        return n;
    }
    void createOutputLatches() override {}
    idx_t getNInputs() const override { return 0; }
    idx_t getNOutputs() const override { return 1; }
    const char *getInputName(idx_t) const override { return nullptr; }
    const char *getOutputName(idx_t) const override { return "wideramp"; }
};

// Minimal RIFF/WAV reader: PCM16 or float32, returns interleaved floats.
static bool readWavFloat(const char *path, std::vector<float> &out,
                         int &channels)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<char> bytes((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
    if (bytes.size() < 44 || std::memcmp(bytes.data(), "RIFF", 4) != 0)
        return false;
    size_t p = 12;
    channels = 0;
    int bits = 0, fmt = 0;
    while (p + 8 <= bytes.size()) {
        std::uint32_t sz;
        std::memcpy(&sz, bytes.data() + p + 4, 4);
        if (std::memcmp(bytes.data() + p, "fmt ", 4) == 0) {
            std::uint16_t v;
            std::memcpy(&v, bytes.data() + p + 8, 2);  fmt = v;
            std::memcpy(&v, bytes.data() + p + 10, 2); channels = v;
            std::memcpy(&v, bytes.data() + p + 22, 2); bits = v;
        } else if (std::memcmp(bytes.data() + p, "data", 4) == 0) {
            size_t start = p + 8, avail = bytes.size() - start;
            size_t n = (sz < avail ? sz : avail);
            if (fmt == 3 && bits == 32) {          // IEEE float
                out.resize(n / 4);
                std::memcpy(out.data(), bytes.data() + start, out.size() * 4);
                return true;
            }
            if (fmt == 1 && bits == 16) {          // PCM16 (what WAVWriter emits)
                out.resize(n / 2);
                for (size_t i = 0; i < out.size(); ++i) {
                    std::int16_t v;
                    std::memcpy(&v, bytes.data() + start + i * 2, 2);
                    out[i] = (float)v / 32768.0f;
                }
                return true;
            }
            return false;
        }
        p += 8 + sz + (sz & 1);
    }
    return false;
}

// Render `c` over [t0, t1) to `path` and wait. Returns whether it completed
// successfully; `error` gets the completion message.
static bool renderTo(std::shared_ptr<twComponent> c, std::uint32_t rate,
                     audio::AudioFormat fmt, int quality, const char *path,
                     double t0, double t1, std::string *error = nullptr,
                     audio::RenderSession::WriterFactory factory = nullptr)
{
    audio::RenderParams p;
    p.outputPath = path;
    p.format = fmt;
    p.quality = quality;
    p.startTimeSec = t0;
    p.endTimeSec = t1;
    std::atomic<bool> done{false}, ok{false};
    std::string msg;
    audio::RenderSession s;
    if (factory) s.setWriterFactory(factory);
    s.onComplete = [&](bool success, const char *e) {
        msg = e ? e : "";
        ok = success;
        done = true;
    };
    if (!s.start(c, p, rate)) {
        if (error) *error = s.errorMessage();
        printf("     start: %s\n", s.errorMessage());
        return false;
    }
    for (int i = 0; i < 600 && !done; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (error) *error = msg;
    return done && ok;
}

static long fileSize(const char *path)
{
    std::FILE *f = std::fopen(path, "rb");
    if (!f) return -1;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fclose(f);
    return n;
}

// A writer whose write() starts failing after `okWrites` calls -- a disk that
// fills up mid-render. Everything else succeeds, so the ONLY failure in the
// render is the one FileSink used to throw away.
class FailingWriter : public audio::AudioFileWriter {
public:
    FailingWriter(int okWrites, std::atomic<int> *calls)
        : okWrites_(okWrites), calls_(calls) {}
    bool open(const std::string &, const audio::AudioFileConfig &) override { return true; }
    bool write(const float *, std::size_t) override {
        const int n = ++*calls_;
        if (n > okWrites_) { err_ = "disk full (FailingWriter)"; return false; }
        return true;
    }
    bool close() override { return true; }
    const char *errorMessage() const override { return err_.c_str(); }
private:
    int okWrites_;
    std::atomic<int> *calls_;
    std::string err_;
};

int main(int argc, char **argv)
{
    const char *outPath =
        (argc > 1) ? argv[1] : "render_module_test.wav";

    tw303aEnvironment env;
    // twComponent calls shared_from_this() in freezePage()/createOutputLatches(),
    // which RenderSession drives internally, so the component must be owned by a
    // std::shared_ptr rather than living on the stack.
    auto comp = std::make_shared<RampComponent>(env);
    comp->init();

    const std::uint32_t rate = (std::uint32_t)env.getSRate();
    // A range that (a) does NOT start at 0 and (b) crosses the 65536-frame
    // page boundary, so both the absolute-position rule and the page state
    // chain are exercised.
    const double t0 = 1.0, t1 = 2.5;
    const long long first = (long long)(t0 * rate);
    const long long total = (long long)((t1 - t0) * rate);

    audio::RenderParams params;
    params.outputPath = outPath;
    params.format = audio::AudioFormat::WAV;
    params.startTimeSec = t0;
    params.endTimeSec = t1;

    std::atomic<bool> done{false};
    std::atomic<bool> ok{false};
    std::uint64_t lastPos = 0;
    audio::RenderSession session;
    session.onComplete = [&](bool success, const char *) {
        ok = success; done = true;
    };
    session.onPosition = [&](std::uint64_t p) { lastPos = p; };

    CHECK(session.start(std::static_pointer_cast<twComponent>(comp), params, rate), "render session starts");
    for (int i = 0; i < 600 && !done; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(done && ok, "render completes successfully");
    CHECK(lastPos == (std::uint64_t)(first + total),
          "onPosition reports ABSOLUTE positions up to the range end");

    std::vector<float> samples;
    int channels = 0;
    CHECK(readWavFloat(outPath, samples, channels), "output WAV parses");
    // ONE channel, because the graph declares one (proposal 36 B5). This
    // asserted `channels == 2` until B5: RenderSession hard-coded
    // config.channels = 2 and duplicated the graph's single mono page into
    // both. RampComponent's getOutputChannels() is the default 1, so the honest
    // file is mono, and RenderParams::channels defaulting to 0 ("ask the
    // graph") is what makes that the default rather than something a caller has
    // to remember.
    CHECK(channels == 1, "output width is the GRAPH's width (mono here)");
    long long frames = (long long)samples.size() / channels;
    CHECK(frames == total, "frame count matches the requested range");

    // Content: frame i must be the material at ABSOLUTE position first+i —
    // the marked-range regression (the bug rendered from 0 instead).
    // Tolerance covers PCM16 quantization (~3e-5); val() spacing is 1e-3.
    auto near = [](float a, float b) { return std::fabs(a - b) < 2e-4f; };
    bool contentOk = frames == total && (long long)samples.size() >= total * channels;
    for (long long i = 0; contentOk && i < total; i += 997)
        contentOk = near(samples[(size_t)(i * channels)], val(first + i));
    CHECK(contentOk, "content matches absolute positions (marked range)");

    // Continuity across the 65536 page boundary inside the range.
    long long b = 65536 - first;   // range-local index of the boundary
    bool boundaryOk = contentOk && b > 0 && b < total
        && near(samples[(size_t)((b - 1) * channels)], val(65535))
        && near(samples[(size_t)(b * channels)], val(65536));
    CHECK(boundaryOk, "no discontinuity at the page boundary");

    std::remove(outPath);

    // ------------------------------------------------------------------
    // Proposal 36 B5 — a SIX-channel graph renders a SIX-channel file, and
    // every channel carries its own audio.
    //
    // The unit-level half of AC B5.3 (the project-level half is the qxa case
    // mc_six_channel). What it pins that the qxa case cannot: the exact
    // per-channel values, from a source whose channel c is a constant
    // 0.6 - 0.1*c, so a channel served from the wrong page channel is off by a
    // full 0.1 rather than by a level band. Six because it is the width the AC
    // names and because it is past every hard-coded 2 in the old sink.
    {
        auto wide = std::make_shared<WideRampComponent>(env);
        wide->init();

        const char *widePath = "render_module_wide_test.wav";
        audio::RenderParams wp;
        wp.outputPath  = widePath;
        wp.format      = audio::AudioFormat::WAV;
        wp.startTimeSec = 0.0;
        wp.endTimeSec   = 0.5;

        std::atomic<bool> wdone{false}, wok{false};
        audio::RenderSession wsession;
        wsession.onComplete = [&](bool ok2, const char *) { wok = ok2; wdone = true; };
        CHECK(wsession.start(std::static_pointer_cast<twComponent>(wide), wp, rate),
              "B5: wide render session starts");
        for (int i = 0; i < 600 && !wdone; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK(wdone && wok, "B5: wide render completes");

        std::vector<float> ws;
        int wch = 0;
        CHECK(readWavFloat(widePath, ws, wch), "B5: wide output WAV parses");
        CHECK(wch == 6, "AC B5.3: a 6-channel graph renders a SIX-channel file");

        bool laddersOk = (wch == 6) && !ws.empty();
        const long long wframes = wch ? (long long)ws.size() / wch : 0;
        for (int c = 0; c < wch && laddersOk; ++c) {
            const float want = 0.6f - 0.1f * (float)c;
            for (long long i = 0; i < wframes && laddersOk; i += 331)
                laddersOk = std::fabs(ws[(size_t)(i * wch + c)] - want) < 2e-4f;
        }
        CHECK(laddersOk,
              "AC B5.3: file channel c is GRAPH channel c, at its own level");

        std::remove(widePath);
    }

    // ------------------------------------------------------------------
    // QBX-145 - MP3 export writes a real MP3. It never had: the dlopen'd
    // MP3Writer's open() always failed, so start() returned false here.
    {
        const char *mp3Path = "render_module_test.mp3";
        std::string err;
        const bool rendered = renderTo(comp, rate, audio::AudioFormat::MP3, 192,
                                       mp3Path, t0, t1, &err);
        CHECK(rendered, "QBX-145: an MP3 render starts and completes");

        SF_INFO info = {};
        SNDFILE *in = rendered ? sf_open(mp3Path, SFM_READ, &info) : nullptr;
        CHECK(in != nullptr, "QBX-145: the MP3 reads back through libsndfile");
        if (in) {
            printf("     MP3: %lld frames (rendered %lld), %d ch, %d Hz\n",
                   (long long)info.frames, total, info.channels, info.samplerate);
            const long long d = (long long)info.frames - total;
            CHECK(d >= -1152 && d <= 1152,
                  "QBX-145: MP3 length within one frame of the render");
            std::vector<float> back((size_t)info.frames * (size_t)info.channels);
            const sf_count_t got = sf_readf_float(in, back.data(), info.frames);
            sf_close(in);
            double a = 0.0, b2 = 0.0;
            long long n = 0;
            for (long long i = 4096; i + 4096 < (long long)got && i < total; ++i) {
                const double v = back[(size_t)(i * info.channels)];
                const double w = val(first + i);
                a += v * v; b2 += w * w; ++n;
            }
            const double rmsGot = n ? std::sqrt(a / n) : 0.0;
            const double rmsWant = n ? std::sqrt(b2 / n) : 1.0;
            printf("     MP3 RMS %.4f, source %.4f\n", rmsGot, rmsWant);
            CHECK(std::fabs(rmsGot - rmsWant) < 0.1 * rmsWant,
                  "QBX-145: MP3 RMS within 10% of the material's");
        }
        std::remove(mp3Path);
    }

    // ------------------------------------------------------------------
    // QBX-145 - RenderParams::quality reaches the writer. It reached none:
    // the OGG slider in the render dialog has been dead since it was added.
    // Vorbis quality 0 vs 10 differs by several times in size.
    {
        const char *q0 = "render_module_q0.ogg";
        const char *q10 = "render_module_q10.ogg";
        const bool r0 = renderTo(comp, rate, audio::AudioFormat::OGG, 0, q0, t0, t1);
        const bool r10 = renderTo(comp, rate, audio::AudioFormat::OGG, 10, q10, t0, t1);
        const long s0 = fileSize(q0), s10 = fileSize(q10);
        printf("     OGG q0 %ld bytes, q10 %ld bytes\n", s0, s10);
        CHECK(r0 && r10 && s0 > 0 && s10 > s0 + s0 / 2,
              "QBX-145: OGG quality 10 is much larger than quality 0");
        std::remove(q0);
        std::remove(q10);
    }

    // ------------------------------------------------------------------
    // QBX-145 - a failing write FAILS THE RENDER. FileSink used to discard
    // writer->write()'s result, so a full disk produced a truncated file and
    // a render reported as successful.
    {
        std::atomic<int> calls{0};
        std::string err;
        const bool rendered = renderTo(
            comp, rate, audio::AudioFormat::WAV, 10, "render_module_fail.wav", t0, t1, &err,
            [&](audio::AudioFormat) {
                return std::unique_ptr<audio::AudioFileWriter>(new FailingWriter(2, &calls));
            });
        printf("     writes attempted: %d, completion message: \"%s\"\n",
               calls.load(), err.c_str());
        CHECK(!rendered, "QBX-145: a render whose writes fail reports FAILURE");
        CHECK(err.find("disk full (FailingWriter)") != std::string::npos,
              "QBX-145: the failure carries the writer's own message");
        CHECK(calls.load() == 3,
              "QBX-145: the first failure is sticky (no write after it)");
    }

    printf(failures ? "\n%d FAILURE(S)\n" : "\nall render tests passed\n",
           failures);
    return failures ? 1 : 0;
}
