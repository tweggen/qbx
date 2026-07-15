// tw/sources module test: reader/loop/grain semantics over a synthetic
// vector-backed source (twCapturingSource) — no files, no Qt event loop.
// The invariants tested here are the ones in sources/CONTRACT.md and
// docs/contracts/POSITION_DOMAINS.md.
#include "tw/sources/twcapturingsource.h"
#include "tw/sources/twsamplereader.h"
#include "tw/sources/twloopreader.h"
#include "tw/sources/twgrainsource.h"
#include "tw/pages/io_vector.h"
#include "tw/graph/tw303aenv.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (cond) { printf("ok   %s\n", msg); }                             \
        else      { printf("FAIL %s\n", msg); ++failures; }                 \
    } while (0)

// Distinguishable, never-zero value for absolute source position p.
static float val(long long p) { return (float)((p % 977) + 1) / 1000.0f; }

// Read n frames from a component into a fresh page; return the samples.
static std::vector<float> pull(twComponent &c, length_t n)
{
    auto page = std::make_shared<twOutputPage>();
    IOVector dest(page, 0, n);
    c.calcOutputTo(dest, 0);
    return std::vector<float>(page->samples.begin(), page->samples.begin() + n);
}

int main()
{
    tw303aEnvironment env;

    const length_t N = 10000;
    std::vector<sample_t> data((size_t)N);
    for (length_t i = 0; i < N; ++i) data[(size_t)i] = val(i);
    twCapturingSource src(std::move(data), N, 1, env.getSRate());

    // --- twRandomSource::read: stateless, zero-fill past the end -----------
    {
        float buf[8];
        length_t got = src.read(5, buf, 8, 0);
        CHECK(got == 8 && buf[0] == val(5) && buf[7] == val(12),
              "source read is stateless and position-exact");
        got = src.read(N - 3, buf, 8, 0);
        CHECK(got == 3 && buf[2] == val(N - 1) && buf[3] == 0.0f
                  && buf[7] == 0.0f,
              "read past end returns partial count and zero-fills");
    }

    // --- twSampleReader: absolute seeks, sequential advance -----------------
    {
        // acquireReader() now returns a shared_ptr: the reader is a twComponent
        // and shared_from_this() (via init()) requires shared ownership.
        auto r = src.acquireReader(env, 100);
        auto a = pull(*r, 4);
        CHECK(a[0] == val(100) && a[3] == val(103),
              "acquireReader initial offset positions the cursor");
        auto b = pull(*r, 4);
        CHECK(b[0] == val(104), "reader advances sequentially");
        r->seekTo(2000);   // ABSOLUTE in the source domain (CONTRACT inv. 1)
        auto c = pull(*r, 2);
        CHECK(c[0] == val(2000), "seekTo is absolute, not base-relative");
    }

    // --- twLoopReader: cut-relative cursor, loop base baked in --------------
    {
        const offset_t base = 300;
        const length_t loop = 50;
        // A twComponent (via twSampleReader) must be shared-owned: init() ->
        // createOutputLatches() calls shared_from_this(). A stack local would
        // throw std::bad_weak_ptr.
        auto lr = std::make_shared<twLoopReader>(env, src, base, loop);
        lr->init();
        lr->seekTo(0);      // CUT-relative (CONTRACT inv. 2)
        auto a = pull(*lr, (length_t)(loop + 10));
        CHECK(a[0] == val(base) && a[(size_t)loop - 1] == val(base + loop - 1),
              "loop reader maps cut-relative 0 to the loop base");
        CHECK(a[(size_t)loop] == val(base) && a[(size_t)loop + 9] == val(base + 9),
              "loop reader wraps back to the base at the segment end");
    }

    // --- twGrainSource: stretched domain -------------------------------------
    {
        twGrainParams identity;   // stretch 1.0
        identity.grainSize = 2048;
        identity.crossfade = 512;
        twGrainParams stretched = identity;
        stretched.stretch = 2.0;

        twGrainSource g1(src, identity);
        twGrainSource g2(src, stretched);
        CHECK(std::llabs((long long)g1.length() - (long long)N) < 4096,
              "identity grain keeps roughly the source length");
        double ratio = (double)g2.length() / (double)g1.length();
        CHECK(ratio > 1.8 && ratio < 2.2,
              "stretch 2.0 roughly doubles the material length");
    }

    printf(failures ? "\n%d FAILURE(S)\n" : "\nall sources tests passed\n",
           failures);
    return failures ? 1 : 0;
}
