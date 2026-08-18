// tw/record: the loopback latency measurement (proposal 21 L6a).
//
// The physical half of a loopback calibration — a cable from an output to an
// input — is Windows-manual by nature and cannot be gated here. The
// MEASUREMENT is a pure function of two buffers, and that is the half that can
// be wrong in ways nobody notices: an off-by-one in the arrival, a peak found
// in the probe's own decay rather than its attack, a confident answer produced
// from a capture with no probe in it at all.
//
// So this gates the arithmetic against SYNTHETIC captures, where the true
// delay is known exactly and can be asserted to the FRAME rather than to a
// tolerance. Everything here runs on any host.

#include "tw/record/loopback_calibration.h"

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <random>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (cond) { printf("ok   %s\n", msg); }                             \
        else      { printf("FAIL %s\n", msg); ++failures; }                 \
    } while (0)

using namespace audio;

namespace {

// A capture of `frames`, `channels` wide, with the probe placed at
// `arriveAt` on `channel` and optional white noise everywhere.
std::vector<float> synthCapture(std::size_t frames, std::uint32_t channels,
                                std::uint32_t channel, std::size_t arriveAt,
                                float noiseRms, float probeAmp = 0.5f,
                                unsigned seed = 1234)
{
    std::vector<float> cap(frames * channels, 0.0f);

    if (noiseRms > 0.0f) {
        std::mt19937 rng(seed);
        std::normal_distribution<float> d(0.0f, noiseRms);
        for (float &v : cap) v = d(rng);
    }

    const std::vector<float> probe = loopbackProbe(64, probeAmp);
    for (std::size_t i = 0; i < probe.size(); ++i) {
        const std::size_t f = arriveAt + i;
        if (f >= frames) break;
        cap[f * channels + channel] += probe[i];
    }
    return cap;
}

}  // namespace

// --- 1. the probe itself -----------------------------------------------------

static void testProbe()
{
    printf("\n-- the probe --\n");

    const std::vector<float> p = loopbackProbe(64, 0.5f);
    CHECK(p.size() == 64, "the probe is the requested length");

    // THE ATTACK MUST BE THE FIRST SAMPLE AND THE LARGEST. The measurement
    // reports the position of the largest |sample| as the arrival, so if the
    // envelope peaked anywhere but sample 0 every measurement would be late by
    // exactly that offset — a systematic error that looks like latency.
    float peak = 0.0f;
    std::size_t peakAt = 0;
    for (std::size_t i = 0; i < p.size(); ++i)
        if (std::fabs(p[i]) > peak) { peak = std::fabs(p[i]); peakAt = i; }
    CHECK(peakAt == 0, "the probe's PEAK is its first sample (the attack is what is timed)");
    CHECK(std::fabs(peak - 0.5f) < 1e-6f, "the peak is the requested amplitude");

    // Alternating sign == broadband. A probe with a DC component could be
    // swallowed by any high-pass in the path.
    bool alternates = true;
    for (std::size_t i = 1; i < 8; ++i)
        if (p[i] * p[i - 1] > 0.0f) alternates = false;
    CHECK(alternates, "successive samples alternate in sign (broadband, no DC)");

    CHECK(loopbackProbe(0).empty(), "a zero-length probe is empty, not a crash");
}

// --- 2. the measurement, against a known delay -------------------------------

static void testExactDelay()
{
    printf("\n-- an EXACT delay is recovered EXACTLY --\n");

    // A digital loopback has no noise at all, so the answer should be exact to
    // the frame. Anything less would mean the arithmetic itself is lossy.
    const std::size_t emitted = 1000;
    const std::int64_t delays[] = {0, 1, 137, 4800, 52000};
    for (std::int64_t d : delays) {
        const std::vector<float> cap =
            synthCapture(96000, 2, 0, emitted + (std::size_t) d, 0.0f);
        const LoopbackResult r =
            loopbackMeasure(cap.data(), 96000, 2, 0, (std::int64_t) emitted);

        char msg[128];
        std::snprintf(msg, sizeof(msg),
                      "delay %lld recovered as %lld", (long long) d,
                      (long long) r.roundTripFrames);
        CHECK(r.found && r.roundTripFrames == d, msg);
    }
}

static void testNoise()
{
    printf("\n-- under noise --\n");

    const std::size_t emitted = 1000;
    const std::int64_t trueDelay = 4800;

    // A loopback at line level against a modest floor: still exact, because
    // the peak is still the peak.
    {
        const std::vector<float> cap =
            synthCapture(96000, 2, 0, emitted + trueDelay, 0.01f, 0.5f);
        const LoopbackResult r = loopbackMeasure(cap.data(), 96000, 2, 0, emitted);
        CHECK(r.found && r.roundTripFrames == trueDelay,
              "exact through a -40 dB noise floor");
        printf("     (peak %.3f, peak/noise %.1f)\n", r.peakAmplitude, r.peakToNoise);
    }

    // A weak return — attenuated probe, same floor. The measurement should
    // still find it, and the ratio should show it is less trustworthy.
    {
        const std::vector<float> cap =
            synthCapture(96000, 2, 0, emitted + trueDelay, 0.01f, 0.08f);
        const LoopbackResult r = loopbackMeasure(cap.data(), 96000, 2, 0, emitted);
        CHECK(r.found && r.roundTripFrames == trueDelay,
              "exact with an attenuated return");
        printf("     (found %d, delay %lld want %lld, peak %.4f, peak/noise %.3f)\n",
               (int) r.found, (long long) r.roundTripFrames, (long long) trueDelay,
               r.peakAmplitude, r.peakToNoise);
    }
}

// --- 3. REFUSING, which is the part that protects the user -------------------

static void testRefusal()
{
    printf("\n-- it REFUSES rather than guessing --\n");

    // THE ONE THAT MATTERS. A capture with no probe in it — nothing plugged
    // in, wrong channel, the cable in the wrong socket — still has a largest
    // sample somewhere. Reporting its position as a latency would write a
    // random number into a setting that shifts every take the user records
    // afterwards.
    {
        std::vector<float> cap(96000 * 2, 0.0f);
        std::mt19937 rng(7);
        std::normal_distribution<float> d(0.0f, 0.01f);
        for (float &v : cap) v = d(rng);
        const LoopbackResult r = loopbackMeasure(cap.data(), 96000, 2, 0, 1000);
        CHECK(!r.found, "noise with NO probe is refused, not measured");
        printf("     (peak/noise %.1f, would have claimed %lld frames)\n",
               r.peakToNoise, (long long) r.roundTripFrames);
    }

    // THE ONE HARDWARE TAUGHT US, and the reason `minPeakAmplitude` exists.
    // A relative SNR test alone passes on a NOISE SPIKE over a quiet floor,
    // which is what an unconnected input actually looks like — real inputs
    // have interference, relays and cables being touched, none of which
    // Gaussian noise models. Measured on a real interface with NO CABLE: a
    // peak of 0.0045 (0.45 % of full scale, 40 dB below the emitted probe)
    // standing 17.9x above the RMS floor, reported as a confident 345.62 ms.
    {
        std::vector<float> cap(96000 * 2, 0.0f);
        std::mt19937 rng(11);
        std::normal_distribution<float> d(0.0f, 0.0002f);
        for (float &v : cap) v = d(rng);
        // One isolated spike: tiny in absolute terms, enormous relative to the
        // floor around it. Exactly the shape that fooled the relative test.
        cap[40000 * 2 + 0] = 0.0045f;

        const LoopbackResult relOnly =
            loopbackMeasure(cap.data(), 96000, 2, 0, 1000, 8.0f, 0.0f);
        CHECK(relOnly.found,
              "a NOISE SPIKE passes the relative test alone (this is the bug)");
        printf("     (relative-only would claim %lld frames from a %.4f peak, "
               "peak/noise %.1f)\n",
               (long long) relOnly.roundTripFrames, relOnly.peakAmplitude,
               relOnly.peakToNoise);

        // With the absolute floor the caller actually uses (1 % of a 0.5
        // probe = 0.005), the same capture is refused.
        const LoopbackResult withAbs =
            loopbackMeasure(cap.data(), 96000, 2, 0, 1000, 8.0f, 0.5f * 0.01f);
        CHECK(!withAbs.found, "the ABSOLUTE floor refuses it");
    }

    // Silence: nothing to find, and no division by a zero floor.
    {
        const std::vector<float> cap(96000 * 2, 0.0f);
        const LoopbackResult r = loopbackMeasure(cap.data(), 96000, 2, 0, 1000);
        CHECK(!r.found, "silence is refused");
    }

    // An arrival BEFORE the emit is impossible, and is what a stale buffer or
    // a mis-stamped emit position looks like. It must not become a negative
    // latency that then gets subtracted from every placement.
    {
        const std::vector<float> cap = synthCapture(96000, 2, 0, 500, 0.0f);
        const LoopbackResult r = loopbackMeasure(cap.data(), 96000, 2, 0, 1000);
        CHECK(!r.found, "an arrival BEFORE the emit is refused");
    }

    // Wrong channel: the probe is on channel 1, we look at channel 0.
    {
        const std::vector<float> cap = synthCapture(96000, 2, 1, 5800, 0.005f);
        const LoopbackResult r0 = loopbackMeasure(cap.data(), 96000, 2, 0, 1000);
        const LoopbackResult r1 = loopbackMeasure(cap.data(), 96000, 2, 1, 1000);
        CHECK(!r0.found, "the wrong channel finds nothing and says so");
        CHECK(r1.found && r1.roundTripFrames == 4800, "the right channel is exact");
    }

    // Degenerate inputs.
    CHECK(!loopbackMeasure(nullptr, 100, 2, 0, 0).found, "a null buffer is refused");
    {
        const std::vector<float> cap(200, 0.0f);
        CHECK(!loopbackMeasure(cap.data(), 100, 2, 5, 0).found,
              "a channel index past the width is refused");
    }
}

// --- 4. what the user is offered ---------------------------------------------

static void testSuggestedOffset()
{
    printf("\n-- the offset OFFERED to the user --\n");

    // THE OFFSET IS THE RESIDUAL, NOT THE ROUND TRIP. The app already
    // compensates the driver's REPORTED latencies automatically (L3b), so
    // handing over the whole round trip would double-count everything the
    // driver got right — which on a well-behaved interface is all of it.
    {
        // A driver that reports honestly: 735 out + 304 in, and the cable
        // measures exactly that. There is nothing left to correct.
        const double ms = loopbackSuggestedOffsetMs(1039, 735, 304, 48000);
        CHECK(std::fabs(ms) < 1e-9, "an HONEST driver yields an offset of ZERO");
    }
    {
        // A driver under-reporting by 480 frames = 10 ms at 48 kHz. Audio
        // arrives later than the app believes, so takes must be placed
        // EARLIER, so the offset is POSITIVE — the app-wide sign convention.
        const double ms = loopbackSuggestedOffsetMs(1039 + 480, 735, 304, 48000);
        CHECK(std::fabs(ms - 10.0) < 1e-9, "under-reporting by 480 frames -> +10 ms");
    }
    {
        // Over-reporting: the correction runs the other way.
        const double ms = loopbackSuggestedOffsetMs(1039 - 240, 735, 304, 48000);
        CHECK(std::fabs(ms + 5.0) < 1e-9, "over-reporting by 240 frames -> -5 ms");
    }
    CHECK(loopbackMs(4800, 48000) == 100.0, "4800 frames at 48k is 100 ms");
    CHECK(loopbackMs(1000, 0) == 0.0, "a zero rate yields 0 rather than a division");
}

int main()
{
    printf("=== loopback_test (proposal 21 L6a: the calibration measurement) ===\n");
    testProbe();
    testExactDelay();
    testNoise();
    testRefusal();
    testSuggestedOffset();

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
