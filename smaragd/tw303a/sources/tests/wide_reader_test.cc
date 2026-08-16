// Proposal 36 B3 — the clip path goes wide. AC B3.1 lives here, plus the
// engine-level half of B3.2 and the mono-degradation checks B3.3 depends on.
//
// WHAT THIS GATE IS FOR. B2 built the machinery that COULD make a page wider
// and deliberately used none of it in production: the only wide pages in the
// tree were the ones two tests built. B3 makes twSampleReader — the component
// every plain clip resolves to — genuinely as wide as its source, so what has
// to be proven here is that a REAL STEREO WAV survives the reader seam with its
// two channels intact, coherent, and at the right position.
//
// THE ASSERTION THAT MATTERS IS THE EXACT ONE. Every page channel is compared
// sample-for-sample against twRandomSource::read() at THE SAME position. That
// is not pedantry: the failure mode §4.3 exists to prevent is a page whose
// channel 1 holds the NEXT page's audio (a per-channel loop over a cursor-
// bearing component), and an RMS or "the channels differ" check passes that
// happily — the levels are right, the audio is a page early. A displacement of
// any amount fails an exact compare.
//
// It lives in sources/tests because the module DAG says so: tw_sources may see
// core/pages/graph/sidecar, and this needs twSampleSource (a file), the reader,
// the loop reader and twWavInput. The scheduler half of AC B3.2 — the resolved
// component of a REAL SCut, driven by the REAL CaptureRevalidator — cannot live
// here (schedule is not in sources' closure, and an SCut is app code); it is the
// qxa case mc_stereo_clip_width, via assert-clip-channels.

#include "tw/sources/twsamplesource.h"
#include "tw/sources/twsamplereader.h"
#include "tw/sources/twloopreader.h"
#include "tw/sources/twwavinput.h"
#include "tw/sources/twgrainsource.h"
#include "tw/sources/twgrainparams.h"
#include "tw/graph/tw303aenv.h"
#include "tw/graph/twlatch.h"
#include "tw/pages/io_vector.h"
#include "tw/pages/tw_output_page.h"
#include "tw/sidecar/twaspects.h"
#include "tw/sidecar/twqaf.h"
#include "tw/sidecar/twsidecarstore.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <system_error>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (cond) { printf("ok   %s\n", msg); }                             \
        else      { printf("FAIL %s\n", msg); ++failures; }                 \
    } while (0)

static const offset_t CAP = (offset_t)twOutputPage::FRAME_CAPACITY;

#ifndef TW_STEREO_FIXTURE
#  error "TW_STEREO_FIXTURE must be defined by the build"
#endif
#ifndef TW_MONO_FIXTURE
#  error "TW_MONO_FIXTURE must be defined by the build"
#endif

// Does channel `c` of `page` hold EXACTLY what the source has at this page's
// position? Sampled densely enough that no plausible displacement survives:
// both page edges, a 100-frame (one whole 480 Hz cycle) grid, and the last
// frame of real material.
static bool channelIsExact(const std::shared_ptr<twOutputPage> &page, idx_t c,
                           const twRandomSource &src, offset_t pagePos,
                           length_t frames)
{
    std::vector<sample_t> want((size_t)frames);
    const_cast<twRandomSource &>(src).read(pagePos, want.data(), frames, c);
    const sample_t *got = page->channelPtr(c);
    for (length_t i = 0; i < frames; ++i) {
        if (got[i] != want[(size_t)i]) {
            printf("     first mismatch ch %d frame %lld: got %.9g want %.9g\n",
                   (int)c, (long long)i, (double)got[i], (double)want[(size_t)i]);
            return false;
        }
    }
    return true;
}

static double rmsOf(const sample_t *p, length_t n)
{
    double s = 0.0;
    for (length_t i = 0; i < n; ++i) s += (double)p[i] * (double)p[i];
    return std::sqrt(s / (double)n);
}

int main()
{
    tw303aEnvironment env;
    env.setSRate(48000);   // the fixtures' native rate: no resampled view

    // =====================================================================
    // AC B3.1 — a stereo WAV's page carries two DISTINCT channels at the
    // reader, asserted at engine level.
    // =====================================================================
    auto stereo = std::make_shared<twSampleSource>(env, QString(TW_STEREO_FIXTURE));
    CHECK(stereo->wasLoaded(), "B3.1: the stereo fixture loads");
    CHECK(stereo->channels() == 2 && stereo->length() == 144000
              && stereo->sampleRate() == 48000,
          "B3.1: …as 2 channels x 144000 frames at 48 kHz");
    if (!stereo->wasLoaded()) return 1;

    {
        auto reader = stereo->acquireReader(env, 0);
        CHECK(reader->getOutputChannels() == 2,
              "B3.1: the reader DECLARES the source's width");
        CHECK(reader->getNOutputs() == 2,
              "B3.1: …and its port count still agrees (they mean the same thing "
              "in THIS class, and only here)");

        // Three pages: 0..65535 and 65536..131071 are entirely real material,
        // 131072..143999 is the tail plus the zero-fill past end. The tail page
        // is included on purpose — a wide render that got its length bound from
        // the wrong channel would show up there first.
        bool widthOk = true, exactOk = true, distinctOk = true;
        double r0[3] = {0, 0, 0}, r1[3] = {0, 0, 0};
        for (int i = 0; i < 3; ++i) {
            const offset_t pos = (offset_t)i * CAP;
            auto p = reader->freezePage(pos, nullptr, 0, (length_t)CAP,
                                        env.getSRate(), nullptr);
            if (!p || p->validAspects == 0) { widthOk = exactOk = false; break; }
            if (p->channels() != 2) widthOk = false;
            // EXACT, against the source at THIS page's position. This is the
            // page-displacement gate.
            if (!channelIsExact(p, 0, *stereo, pos, (length_t)CAP)) exactOk = false;
            if (!channelIsExact(p, 1, *stereo, pos, (length_t)CAP)) exactOk = false;
            r0[i] = rmsOf(p->channelPtr(0), (length_t)CAP);
            r1[i] = rmsOf(p->channelPtr(1), (length_t)CAP);
            // Distinct as CONTENT, not merely as level: no frame of the two
            // channels may be equal-and-nonzero everywhere.
            bool anyDiff = false;
            for (offset_t f = 0; f < CAP; ++f)
                if (p->channelPtr(0)[f] != p->channelPtr(1)[f]) { anyDiff = true; break; }
            if (!anyDiff) distinctOk = false;
        }
        CHECK(widthOk, "B3.1: every frozen page is 2 channels wide");
        CHECK(exactOk,
              "B3.1: each channel holds EXACTLY the source's samples at that "
              "page's position (3 pages x 2 channels x 65536 frames)");
        CHECK(distinctOk, "B3.1: and the two channels are not the same audio");
        // The fixture's 6 dB ladder, so a channel SWAP fails too.
        CHECK(std::fabs(r0[0] - 0.5) < 0.002 && std::fabs(r1[0] - 0.25) < 0.002
                  && std::fabs(r0[1] - 0.5) < 0.002 && std::fabs(r1[1] - 0.25) < 0.002,
              "B3.1: ch0 rms 0.5 and ch1 rms 0.25 on both full pages (a swap fails)");
        CHECK(r0[2] > 0.2 && r0[2] < 0.25 && r1[2] > 0.1 && r1[2] < 0.125,
              "B3.1: the tail page carries the partial material, ladder intact");
    }

    // =====================================================================
    // §4.4 rule (1) on a REAL reader: the per-channel latches stop being dead
    // code. Before B3 every one of these served channel 0.
    // =====================================================================
    {
        auto reader = stereo->acquireReader(env, 0);
        reader->init();
        bool plugOk = true;
        for (idx_t c = 0; c < 2; ++c) {
            twLatchOutput *lo = reader->linkOutput(c);
            if (!lo) { plugOk = false; break; }
            lo->seekStream(0);
            std::vector<sample_t> got(1024), want(1024);
            const length_t n = lo->readData(got.data(), 1024);
            stereo->read(0, want.data(), 1024, c);
            if (n != 1024) { plugOk = false; break; }
            for (length_t i = 0; i < 1024; ++i)
                if (got[(size_t)i] != want[(size_t)i]) { plugOk = false; break; }
        }
        CHECK(plugOk, "B3: plug index c of a stereo reader yields channel c "
                      "(the per-channel latches finally mean something)");
    }

    // =====================================================================
    // TRAP 18 — the narrow degradation. A wide component handed a MONO page,
    // or asked through the legacy IOVector pull, must render channel 0 through
    // the pre-B3 path rather than fall into the base renderFrames() /
    // calcOutputTo() mutual recursion (which exhausts the stack, 0xC00000FD).
    // =====================================================================
    {
        auto reader = stereo->acquireReader(env, 0);
        auto scratch = std::make_shared<twOutputPage>();   // width 1
        CHECK(scratch->channels() == 1, "trap 18: the scratch page is width 1");
        IOVector dest(scratch, 0, 4096);
        const length_t got = reader->calcOutputTo(dest, 0);
        std::vector<sample_t> want(4096);
        stereo->read(0, want.data(), 4096, 0);
        bool ok = (got == 4096);
        for (length_t i = 0; ok && i < 4096; ++i)
            if (scratch->channelPtr(0)[i] != want[(size_t)i]) ok = false;
        CHECK(ok, "trap 18: a wide reader still serves channel 0 through the "
                  "narrow IOVector pull, and does not recurse");

        // …and the same through renderFrames() into a width-1 page buffer:
        // renderFrames() is deliberately NOT overridden, so the base routes to
        // the overridden calcOutputTo() instead of back to itself.
        //
        // A FULL PAGE, not a short block, and that is not laziness. The legacy
        // raw-pointer twComponent::calcOutputTo(sample_t*, length, idx) builds a
        // scratch page, resizeMonoScratch(length)s it, and then wraps it with
        // IOVector::CreateForPageOutput — which hard-codes FRAME_CAPACITY as the
        // length regardless of the resize. For length < FRAME_CAPACITY the
        // component is therefore asked for a whole page, its cursor advances a
        // whole page, and the memcpy back reads past the scratch buffer. That is
        // PRE-EXISTING and latent (the freeze path only ever calls it with
        // FRAME_CAPACITY), it is not B3's to fix, and asserting the shorter call
        // here would only pin a bug. Reported instead.
        std::vector<sample_t> full((size_t)CAP);
        stereo->read(0, full.data(), (length_t)CAP, 0);
        auto mono = std::make_shared<twOutputPage>();
        mono->startPosition = 0;
        reader->seekTo(0);   // the pull above advanced the cursor by 4096
        const length_t n = reader->renderFrames(mono->channelPtr(0), (length_t)CAP,
                                                nullptr, 0, 0);
        bool ok2 = (n == (length_t)CAP);
        for (length_t i = 0; ok2 && i < (length_t)CAP; ++i)
            if (mono->channelPtr(0)[i] != full[(size_t)i]) ok2 = false;
        CHECK(ok2, "trap 18: renderFrames() on a mono buffer degrades to "
                   "channel 0 rather than recursing");
    }

    // =====================================================================
    // twLoopReader must apply the tiling to EVERY channel. Inheriting
    // twSampleReader::renderPageWide() would give a looping stereo clip a
    // single linear pass — audible, and invisible to any width-1 gate.
    // =====================================================================
    {
        const offset_t base = 1000;
        const length_t loopLen = 7000;   // not a page divisor, so the tiling wraps
        auto lr = std::make_shared<twLoopReader>(env, *stereo, base, loopLen);
        lr->init();
        CHECK(lr->getOutputChannels() == 2,
              "B3: a loop reader inherits its source's width");

        auto p = lr->freezePage(0, nullptr, 0, (length_t)CAP, env.getSRate(),
                                nullptr);
        bool ok = p && p->validAspects != 0 && p->channels() == 2;
        // Ground truth: the tiling, computed independently of the component.
        if (ok) {
            for (idx_t c = 0; c < 2 && ok; ++c) {
                std::vector<sample_t> want((size_t)CAP);
                for (offset_t f = 0; f < CAP; ++f) {
                    sample_t one = 0.0f;
                    stereo->read(base + (f % loopLen), &one, 1, c);
                    want[(size_t)f] = one;
                }
                for (offset_t f = 0; f < CAP; ++f)
                    if (p->channelPtr(c)[f] != want[(size_t)f]) {
                        printf("     loop ch %d differs at frame %lld\n",
                               (int)c, (long long)f);
                        ok = false;
                        break;
                    }
            }
        }
        CHECK(ok, "B3: a looping stereo clip tiles BOTH channels, exactly");

        // And a loop reader with no window is a plain reader on every channel.
        auto lr0 = std::make_shared<twLoopReader>(env, *stereo, 0, 0);
        lr0->init();
        auto p0 = lr0->freezePage(CAP, nullptr, 0, (length_t)CAP, env.getSRate(),
                                  nullptr);
        CHECK(p0 && p0->channels() == 2
                  && channelIsExact(p0, 1, *stereo, CAP, (length_t)CAP),
              "B3: loopLen <= 0 degrades to the linear wide read, channel 1 "
              "included");
    }

    // =====================================================================
    // twWavInput — §7 trap 4. getNOutputs() returned a hardcoded 4 with ONE
    // latch built. It is now the source's channel count, and the component is
    // as wide as the file, so the SCut::resolveClip fallback (content root
    // component, before a reader exists) no longer narrows a stereo file.
    // =====================================================================
    {
        auto wav = std::make_shared<twWavInput>(env, QString(TW_STEREO_FIXTURE));
        wav->init();
        CHECK(wav->wasLoaded(), "trap 4: twWavInput loads the stereo fixture");
        CHECK(wav->getNOutputs() == 2,
              "trap 4: getNOutputs() is the source's channel count, not 4");
        CHECK(wav->getOutputChannels() == 2, "trap 4: …and so is its page width");
        CHECK(wav->linkOutput(0) != nullptr && wav->linkOutput(1) != nullptr,
              "trap 4: a latch exists for every port it claims");

        auto p = wav->freezePage(0, nullptr, 0, (length_t)CAP, env.getSRate(),
                                 nullptr);
        CHECK(p && p->validAspects != 0 && p->channels() == 2
                  && channelIsExact(p, 0, *stereo, 0, (length_t)CAP)
                  && channelIsExact(p, 1, *stereo, 0, (length_t)CAP),
              "trap 4: a wav input's page carries both channels, exactly");
    }

    // =====================================================================
    // AC B3.3's precondition — MONO IS UNCHANGED. A width-1 source declares 1,
    // allocates the page it always allocated, and takes the pre-B3 render path:
    // renderPageWide() is never called for it. That is what makes the byte
    // gate meaningful rather than lucky.
    // =====================================================================
    {
        auto mono = std::make_shared<twSampleSource>(env, QString(TW_MONO_FIXTURE));
        CHECK(mono->wasLoaded() && mono->channels() == 1,
              "B3.3: the mono fixture loads as 1 channel");
        if (mono->wasLoaded()) {
            auto reader = mono->acquireReader(env, 0);
            CHECK(reader->getOutputChannels() == 1,
                  "B3.3: a mono source declares width 1");
            auto p = reader->freezePage(0, nullptr, 0, (length_t)CAP,
                                        env.getSRate(), nullptr);
            CHECK(p && p->channels() == 1
                      && p->sampleCount() == (size_t)CAP,
                  "B3.3: …and allocates exactly the page it always allocated");
            CHECK(p && channelIsExact(p, 0, *mono, 0, (length_t)CAP),
                  "B3.3: mono content is byte-identical through the narrow path");

            auto wav = std::make_shared<twWavInput>(env, QString(TW_MONO_FIXTURE));
            wav->init();
            CHECK(wav->getNOutputs() == 1 && wav->getOutputChannels() == 1,
                  "B3.3: a mono wav input is width 1 (it used to claim 4 ports)");
        }
    }

    // =====================================================================
    // AC B3.4 — "no old sidecar entry is a wrong-shape hit; assert a MISS,
    // not a wrong hit."
    //
    // NOTHING NEEDED BUMPING, and this is the check that says so rather than
    // asserting it. The warp.pcm key already carries the channel count as
    // normative field 3 of the params blob, and load() re-checks qi.channels,
    // qi.sourceRate, qi.sourceFrames and qi.payloadLen before adopting an
    // entry. B3 also changes no INPUT to that key: twGrainSource has always
    // taken channels_ = src.channels(), so a stereo file's warp was already
    // stored 2 channels wide. Same for the analysis aspects (onsets / loudness
    // / f0), which have always been computed over every channel with
    // qi.channels = nCh.
    //
    // The forgery below isolates the qi.channels check as the sole
    // discriminator: both doctored entries sit at the SAME key with the SAME
    // payload length and frame count, and differ only in the channel field and
    // the bytes. The CONTROL (right channel count) must be ADOPTED — without
    // it, "the wrong one was not adopted" would prove only that the forgery
    // never reached the adoption path.
    {
        namespace fs = std::filesystem;
        const fs::path root = fs::temp_directory_path() / "smaragd_b34_warpkey";
        std::error_code ec;
        fs::remove_all(root, ec);
        twSidecarStore::instance().setRoot(root.string());
        CHECK(twSidecarStore::instance().enabled(),
              "B3.4: the test store is live");

        // A STACK twSampleSource: sharedRef() returns null, so the grain source
        // takes the MATERIALIZED path — the only one that writes warp.pcm at
        // all (the streaming path renders on demand and caches nothing).
        twSampleSource stack(env, QString(TW_STEREO_FIXTURE));
        twGrainParams gp;
        gp.stretch = Fraction(5, 4);

        length_t warpFrames = 0;
        {
            twGrainSource g(stack, gp);
            warpFrames = g.length();
            CHECK(g.channels() == 2 && warpFrames == 180000,
                  "B3.4: a stereo warp is built 2 channels wide (x1.25)");
        }

        auto rd = twSidecarStore::instance().loadAny(
            stack.contentHash(), twAspect::WarpPcm, twAspect::WarpPcmVersion);
        CHECK(rd != nullptr, "B3.4: the warp was cached");
        if (rd) {
            twQafInfo base = rd->info();
            CHECK(base.channels == 2,
                  "B3.4: …and the entry records the source's channel count");
            const uint64_t payloadFloats = base.recordCount;
            rd.reset();   // release the handle before overwriting the file

            // CONTROL: a correctly-shaped entry IS adopted, so the forgery is
            // known to reach the adoption path.
            {
                std::vector<float> forged((size_t)payloadFloats, 0.25f);
                twQafInfo ok = base;
                twSidecarStore::instance().store(
                    ok, forged.data(), payloadFloats * sizeof(float));
                twGrainSource g(stack, gp);
                sample_t buf[8] = {0};
                g.read(0, buf, 8, 0);
                CHECK(buf[0] == 0.25f && buf[7] == 0.25f,
                      "B3.4 (control): a right-shaped entry is adopted");
            }

            // THE MISS: identical key, identical payload length and frame
            // count, channel field wrong. Must be refused and recomputed.
            {
                std::vector<float> forged((size_t)payloadFloats, 0.5f);
                twQafInfo wrong = base;
                wrong.channels = 1;
                twSidecarStore::instance().store(
                    wrong, forged.data(), payloadFloats * sizeof(float));
                twGrainSource g(stack, gp);
                sample_t buf[8] = {0};
                g.read(0, buf, 8, 0);
                bool adopted = true;
                for (int i = 0; i < 8; ++i) if (buf[i] != 0.5f) adopted = false;
                CHECK(!adopted,
                      "B3.4: an entry whose channel count disagrees is a MISS, "
                      "not a wrong-shape hit");
            }
        }

        twSidecarStore::instance().setRoot("");
        fs::remove_all(root, ec);
    }

    // The base renderPageWide() must never have refused: every wide component
    // exercised above overrides it. A refusal here would mean a page was
    // published as silence.
    CHECK(twComponent::wideRenderRefusals() == 0,
          "B3: no component was asked to fill a wide page it cannot fill");

    printf(failures ? "\nFAILED (%d)\n" : "\nPASSED\n", failures);
    return failures ? 1 : 0;
}
