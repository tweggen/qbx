// tw/render module test: RenderSession end-to-end against a scripted
// component — absolute-position ranges (a marked range does NOT start at
// project 0), page-boundary continuity, and file completeness. Normative
// background: render/CONTRACT.md, FREEZE_PROTOCOL.md, POSITION_DOMAINS.md
// rule 6.
#include "tw/render/render_session.h"
#include "tw/graph/twcomponent.h"
#include "tw/graph/tw303aenv.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <fstream>
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

int main(int argc, char **argv)
{
    const char *outPath =
        (argc > 1) ? argv[1] : "render_module_test.wav";

    tw303aEnvironment env;
    RampComponent comp(env);
    comp.init();

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

    CHECK(session.start(&comp, params, rate), "render session starts");
    for (int i = 0; i < 600 && !done; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(done && ok, "render completes successfully");
    CHECK(lastPos == (std::uint64_t)(first + total),
          "onPosition reports ABSOLUTE positions up to the range end");

    std::vector<float> samples;
    int channels = 0;
    CHECK(readWavFloat(outPath, samples, channels), "output WAV parses");
    CHECK(channels == 2, "output is stereo (duplicated mono)");
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
    printf(failures ? "\n%d FAILURE(S)\n" : "\nall render tests passed\n",
           failures);
    return failures ? 1 : 0;
}
