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
    return std::vector<float>(page->channelPtr(0), page->channelPtr(0) + n);
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

    // --- a WIDE capture (proposal 36 B7) -------------------------------------
    //
    // twCapturingSource has carried a `channels` parameter since proposal 07 and
    // every caller passed 1, so its planar arithmetic at width > 1 had never
    // been executed by anything. B7 makes SCut::buildCapture_ pass the real
    // width, which puts a multi-plane capture on the container/asset PLAYBACK
    // path — so the stride is now load-bearing and gets a test.
    {
        const length_t M = 4096;
        const idx_t CH = 4;
        // Plane c holds val(p) scaled by (c+1): four distinguishable signals
        // that a stride mistake mixes up rather than merely shifts.
        std::vector<sample_t> planar((size_t)CH * (size_t)M);
        for (idx_t c = 0; c < CH; ++c)
            for (length_t i = 0; i < M; ++i)
                planar[(size_t)c * (size_t)M + (size_t)i] = val(i) * (float)(c + 1);

        twCapturingSource wide(std::move(planar), M, CH, env.getSRate());
        CHECK(wide.channels() == CH && wide.length() == M,
              "wide capture reports its channel count and length");

        bool planesOk = true;
        for (idx_t c = 0; c < CH; ++c) {
            float buf[4];
            wide.read(1000, buf, 4, c);
            for (int k = 0; k < 4; ++k)
                if (buf[k] != val(1000 + k) * (float)(c + 1)) planesOk = false;
        }
        CHECK(planesOk, "each capture plane reads its own signal at its own offset");

        // "mono plays on every channel": an out-of-range channel clamps to the
        // last one, exactly as twSampleSource::read has always done.
        {
            float buf[2];
            wide.read(1000, buf, 2, (idx_t)(CH + 3));
            CHECK(buf[0] == val(1000) * (float)CH,
                  "out-of-range channel clamps to the last plane");
        }

        // The whole point: a reader over the capture is as wide as the capture,
        // so the page a container/asset clip freezes carries every channel.
        auto r = wide.acquireReader(env, 0);
        CHECK(r->getOutputChannels() == CH,
              "a reader over a wide capture declares the capture's width");

        auto page = std::make_shared<twOutputPage>((std::uint16_t)CH);
        r->seekTo(0);
        length_t got = r->renderPageWide(*page, 512, nullptr, 0);
        bool pageOk = (got == 512);
        for (idx_t c = 0; c < CH && pageOk; ++c) {
            const float *p = page->channelPtr(c);
            if (p[0] != val(0) * (float)(c + 1) || p[511] != val(511) * (float)(c + 1))
                pageOk = false;
        }
        CHECK(pageOk, "a wide page over a wide capture carries all four planes coherently");
    }

    printf(failures ? "\n%d FAILURE(S)\n" : "\nall sources tests passed\n",
           failures);
    return failures ? 1 : 0;
}
