// twOutputPage's channel dimension (proposal 36 §4.1, milestone B1b).
//
// AC B1b.4 and AC B1b.5 live here. The reason this test exists at all is stated
// in the proposal and is worth repeating where the code is: B1b converts every
// consumer of a page's sample buffer to an explicit accessor while every page in
// the system is still ONE channel wide. At width 1 a wrong conversion is
// invisible — the byte-exactness gate passes it, and so does the grep. So the
// only instrument that can see one is a page that really has four channels, with
// four signals that cannot be mistaken for each other, read back through the
// accessor with the same arithmetic the engine uses.
//
// What is deliberately NOT here: any use of the scheduler, the freeze path or a
// graph. At B1b nothing in the tree declares a width > 1; this is a unit probe.
// B2 promotes twSyntheticWideSource to a real graph participant.

#include "tw/graph/tw303aenv.h"
#include "tw/pages/io_vector.h"
#include "tw/pages/tw_output_page.h"
#include "tw/pages/tw_page_accounting.h"

#include "tw_synthetic_wide_source.h"

#include <cstdio>
#include <memory>
#include <type_traits>

static int failures = 0;
#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (cond) { printf("ok   %s\n", msg); }                             \
        else      { printf("FAIL %s\n", msg); ++failures; }                 \
    } while (0)

// ---------------------------------------------------------------------------
// AC B1b.5 — `channels` cannot be mutated after allocation, DEMONSTRATED BY THE
// TYPE. Not by a comment, and not by a runtime check that a caller could skip.
//
// Three independent statements, because each closes a different door:
//   (a) `page.channels = 2` does not compile — channels() is a function and the
//       buffer is private, so there is no member to assign to;
//   (b) there is no setChannels() to call;
//   (c) a whole page cannot be assigned over, so no width can arrive that way.
// (a) and (b) are detection idioms: they FAIL TO COMPILE if the door reopens,
// which is the only way to assert the absence of an operation.
// ---------------------------------------------------------------------------
template <class T, class = void>
struct channels_assignable : std::false_type {};
template <class T>
struct channels_assignable<
    T, std::void_t<decltype(std::declval<T &>().channels = std::uint16_t(2))>>
    : std::true_type {};

template <class T, class = void>
struct has_set_channels : std::false_type {};
template <class T>
struct has_set_channels<
    T, std::void_t<decltype(std::declval<T &>().setChannels(std::uint16_t(2)))>>
    : std::true_type {};

static_assert(!channels_assignable<twOutputPage>::value,
              "twOutputPage::channels must not be an assignable member");
static_assert(!has_set_channels<twOutputPage>::value,
              "twOutputPage must not have a channel setter");
static_assert(!std::is_copy_assignable<twOutputPage>::value,
              "a twOutputPage must not be assignable over");
static_assert(!std::is_move_assignable<twOutputPage>::value,
              "a twOutputPage must not be move-assignable");

int main()
{
    const size_t CAP = twOutputPage::FRAME_CAPACITY;   // 65536, per channel
    tw303aEnvironment env;

    // -----------------------------------------------------------------
    // A width-1 page behaves EXACTLY as it did before this milestone.
    // This is the half of AC B1b.4 that guards the 99% case: every page the
    // engine allocates today comes through this default constructor.
    // -----------------------------------------------------------------
    {
        auto p = std::make_shared<twOutputPage>();
        CHECK(p->channels() == 1, "a default-constructed page is one channel wide");
        CHECK(p->channelFrames() == CAP, "…with FRAME_CAPACITY frames");
        CHECK(p->sampleCount() == CAP, "…and FRAME_CAPACITY floats of storage");
        CHECK(p->accountedBytes() == CAP * sizeof(float),
              "…accounting the same 262144 bytes as before B1b");
        // (The getDataPtr() assertion that stood here retired with the accessor
        // itself at B9 — it was the only caller in the tree, and it asserted a
        // width-blind pointer's identity with channel 0.)

        p->channelPtr(0)[0] = 0.5f;
        p->channelPtr(0)[CAP - 1] = -0.25f;
        const twOutputPage &cp = *p;
        CHECK(cp.channelPtr(0)[0] == 0.5f && cp.channelPtr(0)[CAP - 1] == -0.25f,
              "a width-1 page round-trips through the const accessor");

        p->fillSilence();
        CHECK(p->channelPtr(0)[0] == 0.0f && p->channelPtr(0)[CAP - 1] == 0.0f,
              "fillSilence() zeroes the whole buffer");
    }

    // -----------------------------------------------------------------
    // Geometry of a 4-channel page: storage, stride, accounting.
    // -----------------------------------------------------------------
    std::shared_ptr<twOutputPage> wide;
    {
        const auto before = tw::pages::PageAccounting::global();
        wide = std::make_shared<twOutputPage>(twSyntheticWideSource::kChannels);
        const auto after = tw::pages::PageAccounting::global();

        CHECK(wide->channels() == 4, "a page can be built four channels wide");
        CHECK(wide->channelFrames() == CAP,
              "channelFrames() is PER CHANNEL and unchanged by the width");
        CHECK(wide->sampleCount() == 4 * CAP, "storage is channels * FRAME_CAPACITY");
        CHECK(after.bytes - before.bytes == 4 * CAP * sizeof(float),
              "page accounting scales with the width (4 x 262144 B)");
        CHECK(after.pages - before.pages == 1, "…as ONE page, not four");

        // The stride is the CONSTANT FRAME_CAPACITY, never validFrames-relative.
        // Pin it against validFrames explicitly: set a short tail and re-check.
        bool strideOk = true;
        for (idx_t c = 0; c < 4; ++c) {
            if (wide->channelPtr(c) - wide->channelPtr(0) != (ptrdiff_t)(c * CAP)) {
                strideOk = false;
            }
        }
        CHECK(strideOk, "channelPtr(c) - channelPtr(0) == c * CHANNEL_STRIDE");
        CHECK(twOutputPage::CHANNEL_STRIDE == twOutputPage::FRAME_CAPACITY,
              "CHANNEL_STRIDE is FRAME_CAPACITY, a compile-time constant");

        wide->validFrames = 17;
        CHECK(wide->channelPtr(3) - wide->channelPtr(0) == (ptrdiff_t)(3 * CAP),
              "the stride does NOT follow validFrames (a 17-frame tail moves nothing)");
        wide->validFrames = 0;

        CHECK(wide->channelPtr(3) + CAP == wide->channelPtr(0) + wide->sampleCount(),
              "the last channel ends exactly at the end of the storage");
    }

    // -----------------------------------------------------------------
    // AC B1b.4 — the round trip. Four unambiguously distinguishable channels,
    // written by the synthetic component and read back through channelPtr().
    // -----------------------------------------------------------------
    auto synth = std::make_shared<twSyntheticWideSource>(env);
    const offset_t START = 3 * (offset_t)CAP;   // a non-zero page position

    {
        const length_t n = synth->renderWide(*wide, START, (length_t)CAP);
        CHECK(n == (length_t)CAP, "the synthetic component fills a whole page");
        CHECK(synth->seekCount() == 1 && synth->advanceCount() == 1,
              "…from ONE seek and ONE cursor advance, not one per channel (§4.3)");
        CHECK(synth->cursor() == START + (offset_t)CAP,
              "…leaving the cursor exactly one page on");

        // Every frame of every channel, exact. This is the whole point: not a
        // spot check, because a stride error can be arbitrarily localized.
        bool exact = true;
        offset_t firstBadC = -1, firstBadI = -1;
        for (idx_t c = 0; c < 4 && exact; ++c) {
            const float *src = wide->channelPtr(c);
            for (size_t i = 0; i < CAP; ++i) {
                if (src[i] != twSyntheticWideSource::value(c, START + (offset_t)i)) {
                    exact = false; firstBadC = c; firstBadI = (offset_t)i;
                    break;
                }
            }
        }
        if (!exact) {
            printf("     first mismatch at channel %lld frame %lld\n",
                   (long long)firstBadC, (long long)firstBadI);
        }
        CHECK(exact, "every frame of every channel round-trips EXACTLY");
    }

    // The LAST frame of each channel, named separately, because that is the
    // sample a stride-off-by-one would corrupt first and the one an "almost
    // right" arithmetic reaches by falling into the next channel.
    {
        bool lastOk = true, firstOk = true;
        for (idx_t c = 0; c < 4; ++c) {
            if (wide->channelPtr(c)[CAP - 1]
                != twSyntheticWideSource::value(c, START + (offset_t)CAP - 1)) {
                lastOk = false;
            }
            if (wide->channelPtr(c)[0] != twSyntheticWideSource::value(c, START)) {
                firstOk = false;
            }
        }
        CHECK(lastOk, "the LAST frame of every channel is that channel's own sample");
        CHECK(firstOk, "…and so is the first");

        // Adjacency, stated as the thing that would actually go wrong: the last
        // frame of channel c and the first frame of channel c+1 are neighbours in
        // memory and must belong to DIFFERENT channels.
        bool neighboursDiffer = true;
        for (idx_t c = 0; c + 1 < 4; ++c) {
            const float *end = wide->channelPtr(c) + CAP - 1;
            const float *next = wide->channelPtr(c + 1);
            if (end + 1 != next) neighboursDiffer = false;          // contiguity
            if (*end == *next) neighboursDiffer = false;            // and distinct
        }
        CHECK(neighboursDiffer,
              "channel c's last frame abuts channel c+1's first, and they differ");
    }

    // -----------------------------------------------------------------
    // "Distinguishable" made into a real claim: identify each channel from its
    // samples alone, two independent ways, and check the identification is
    // UNIQUE — a swap of two channels must fail this, which "the channels
    // differ" would not catch.
    // -----------------------------------------------------------------
    {
        float lo[4], hi[4];
        long long flips[4];
        for (idx_t c = 0; c < 4; ++c) {
            const float *src = wide->channelPtr(c);
            lo[c] = src[0]; hi[c] = src[0]; flips[c] = 0;
            for (size_t i = 1; i < CAP; ++i) {
                if (src[i] < lo[c]) lo[c] = src[i];
                if (src[i] > hi[c]) hi[c] = src[i];
                if (src[i] != src[i - 1]) ++flips[c];
            }
        }

        bool identifiedUniquely = true;
        for (idx_t c = 0; c < 4; ++c) {
            int matches = 0;
            for (idx_t k = 0; k < 4; ++k) {
                const float expLo = twSyntheticWideSource::value(k, 8 << k);  // low half
                const float expHi = twSyntheticWideSource::value(k, 0);       // high half
                const long long expFlips =
                    (long long)CAP / twSyntheticWideSource::halfPeriod(k) - 1;
                if (lo[c] == expLo && hi[c] == expHi && flips[c] == expFlips) {
                    ++matches;
                    if (k != c) identifiedUniquely = false;   // matched the WRONG channel
                }
            }
            if (matches != 1) identifiedUniquely = false;
        }
        CHECK(identifiedUniquely,
              "each channel matches its OWN signature and no other channel's");

        // …and the ranges do not overlap, so even a single sample names its
        // channel. That is what makes a mis-plumbed channel impossible to miss.
        bool disjoint = true;
        for (idx_t a = 0; a < 4; ++a) {
            for (idx_t b = 0; b < 4; ++b) {
                if (a != b && !(hi[a] < lo[b] || hi[b] < lo[a])) disjoint = false;
            }
        }
        CHECK(disjoint, "the four channels' value ranges are pairwise disjoint");
    }

    // -----------------------------------------------------------------
    // Writing one channel disturbs no other. The failure this catches is a
    // stride that is too SHORT (bleeding into the next channel) or too LONG
    // (writing past the buffer, which the sentinel below cannot see but ASan
    // would; the short case is the realistic one).
    // -----------------------------------------------------------------
    {
        auto p = std::make_shared<twOutputPage>(4);
        for (idx_t c = 0; c < 4; ++c) {
            float *dst = p->channelPtr(c);
            for (size_t i = 0; i < CAP; ++i) dst[i] = (float)(c + 1);
        }
        // Overwrite channel 1 only, over its whole extent.
        float *one = p->channelPtr(1);
        for (size_t i = 0; i < CAP; ++i) one[i] = -7.5f;

        CHECK(p->channelPtr(0)[CAP - 1] == 1.0f,
              "channel 0's last frame survives a full write of channel 1");
        CHECK(p->channelPtr(2)[0] == 3.0f,
              "channel 2's first frame survives it too");
        CHECK(p->channelPtr(1)[0] == -7.5f && p->channelPtr(1)[CAP - 1] == -7.5f,
              "…and channel 1 really was written end to end");
    }

    // -----------------------------------------------------------------
    // Degenerate inputs answer safely and LOUDLY (§7 trap 9: not with a
    // Q_ASSERT, which this build compiles out).
    // -----------------------------------------------------------------
    {
        CHECK(wide->channelPtr(4) == wide->channelPtr(0)
                  && wide->channelPtr(-1) == wide->channelPtr(0),
              "an out-of-range channel is answered with channel 0, never OOB");

        auto zero = std::make_shared<twOutputPage>(0);
        CHECK(zero->channels() == 1 && zero->sampleCount() == CAP,
              "a zero-channel page is refused and allocated as width 1");

        auto scratch = std::make_shared<twOutputPage>();
        scratch->resizeMonoScratch(1000);
        CHECK(scratch->channelFrames() == 1000 && scratch->sampleCount() == 1000,
              "the mono scratch path still shrinks a width-1 page");
        CHECK(scratch->accountedBytes() == CAP * sizeof(float),
              "…without unbalancing the accounting it reported at construction");

        auto wideScratch = std::make_shared<twOutputPage>(4);
        wideScratch->resizeMonoScratch(1000);
        CHECK(wideScratch->channelFrames() == CAP
                  && wideScratch->sampleCount() == 4 * CAP,
              "a scratch resize of a WIDE page is refused, not silently applied");
    }

    // -----------------------------------------------------------------
    // §4.6: IOVector stays MONO — a view over one channel. Pinned here so the
    // decision is a test rather than a paragraph: an IOVector over a 4-channel
    // page sees channel 0 and nothing else.
    // -----------------------------------------------------------------
    {
        auto src = std::make_shared<twOutputPage>(4);
        for (idx_t c = 0; c < 4; ++c) {
            float *dst = src->channelPtr(c);
            for (size_t i = 0; i < 64; ++i) dst[i] = (float)(c + 1) * 10.0f;
        }
        auto dstPage = std::make_shared<twOutputPage>();
        IOVector out(dstPage, 0, 64);
        const length_t copied = out.copyFrom(IOVector::CreateForPageOutput(src), 0, 64);
        CHECK(copied == 64, "IOVector copies from a wide page");
        CHECK(dstPage->channelPtr(0)[0] == 10.0f && dstPage->channelPtr(0)[63] == 10.0f,
              "…and what it copied is channel 0 (IOVector is a mono view, §4.6)");
    }

    // -----------------------------------------------------------------
    // The accounting balances again once the wide pages are gone: a width
    // dimension that leaked bytes would show up in B9.2's measurement as a
    // number nobody could explain.
    // -----------------------------------------------------------------
    {
        const auto before = tw::pages::PageAccounting::global();
        {
            auto a = std::make_shared<twOutputPage>(8);
            auto b = std::make_shared<twOutputPage>(2);
            const auto during = tw::pages::PageAccounting::global();
            CHECK(during.bytes - before.bytes == 10 * CAP * sizeof(float),
                  "an 8-channel and a 2-channel page account for 10 channels");
        }
        const auto after = tw::pages::PageAccounting::global();
        CHECK(after.bytes == before.bytes && after.pages == before.pages,
              "…and both are fully released when they die");
    }

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
