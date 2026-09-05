// tw/devices module test: the CROSS-PLATFORM half of the ASIO backend
// (proposal 35 Phase 2).
//
// Everything a real ASIO driver touches is Windows-manual by nature — you
// cannot enumerate, open or run one from a headless suite, and the gate for
// that is `asio_probe` plus the runbook in docs/ASIO_WINDOWS_GATE.md. What CAN
// be gated is the arithmetic and the string handling around it, and those were
// deliberately written as SDK-free, Windows-free headers so that this file
// builds and runs on macOS and Linux too:
//
//   1. asio_id      — device-id routing, including the BARE-ID FALLBACK that
//                     keeps every persisted audio/deviceId working.
//   2. asio_bufsize — the ASIOGetBufferSize granularity walk. The gate driver
//                     (a Tascam US-16x08) reports min == max == preferred ==
//                     256 with granularity 0, so it exercises exactly ONE of
//                     the three branches; the other two have no hardware
//                     behind them anywhere and this is their only coverage.
//   3. asio_convert — de-interleaved float <-> Int16/Int24/Int32/Float32/64,
//                     including PACKED Int24, which twSampleType has no
//                     equivalent for.
//
// THERE IS NO RING TEST HERE, and that is the point: proposal 35's file table
// lists an `spsc_ring.h` for the input half, and writing one was a mistake.
// `tw/devices/audio_ring.h` is ALREADY a lock-free SPSC ring — head_/tail_
// atomics with acquire/release and no mutex anywhere — and it is already
// driven by the WASAPI, ALSA and file capture threads, which is the same shape
// an ASIO `bufferSwitch` has. Phase 3 uses it. Its own gate, including the
// tail-drop regression, is in `devices_input_test`.
//
// This test measures nothing wall-clock and opens no device, so unlike
// devices_midi_test and devices_input_test it is NOT RUN_SERIAL.

#include "asio_bufsize.h"
#include "asio_channels.h"
#include "asio_convert.h"
#include "asio_id.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (cond) { printf("ok   %s\n", msg); }                             \
        else      { printf("FAIL %s\n", msg); ++failures; }                 \
    } while (0)

using namespace audio;

// --- 1. device-id routing ----------------------------------------------------

static void testDeviceIds()
{
    printf("\n-- asio_id: routing --\n");

    struct Row {
        const char  *id;
        DeviceIdKind kind;
        const char  *native;
        const char  *what;
    };

    // THE BARE-ID ROWS ARE THE POINT. Every audio/deviceId written before the
    // dispatcher existed is a bare WASAPI endpoint id; if one of those stopped
    // routing to WASAPI, every existing user would silently lose their device
    // selection on upgrade.
    const Row rows[] = {
        {"", DeviceIdKind::Default, "default", "empty -> default"},
        {"default", DeviceIdKind::Default, "default", "\"default\" -> default"},
        {"{0.0.0.00000000}.{abc}", DeviceIdKind::Wasapi, "{0.0.0.00000000}.{abc}",
         "BARE endpoint id -> WASAPI, unchanged"},
        {"wasapi:{0.0.0.00000000}.{abc}", DeviceIdKind::Wasapi, "{0.0.0.00000000}.{abc}",
         "wasapi: prefix stripped"},
        {"WASAPI:{xyz}", DeviceIdKind::Wasapi, "{xyz}", "prefix match is case-insensitive"},
        {"asio:{FA12DE15-482E-4214-8D11-6817497635C0}", DeviceIdKind::Asio,
         "{FA12DE15-482E-4214-8D11-6817497635C0}", "asio: prefix stripped"},
        {"ASIO:{fa12de15}", DeviceIdKind::Asio, "{fa12de15}",
         "asio prefix case-insensitive, payload NOT normalised"},
        {"asio:", DeviceIdKind::Asio, "", "asio: alone -> the first registered driver"},
        {"asio:US-16x08 ASIO Driver", DeviceIdKind::Asio, "US-16x08 ASIO Driver",
         "a driver NAME is a legal payload"},
        {"some-legacy-thing", DeviceIdKind::Wasapi, "some-legacy-thing",
         "unrecognised spelling -> WASAPI (the fallback rule)"},
    };

    for (const Row &r : rows) {
        const ParsedDeviceId p = parseDeviceId(r.id);
        const bool ok = p.kind == r.kind && p.native == r.native;
        CHECK(ok, r.what);
        if (!ok)
            printf("       id='%s' -> kind=%d native='%s' (wanted kind=%d native='%s')\n",
                   r.id, (int) p.kind, p.native.c_str(), (int) r.kind, r.native);
    }

    printf("\n-- asio_id: round trip --\n");
    CHECK(makeDeviceId(DeviceIdKind::Asio, "{abc}") == "asio:{abc}", "make asio id");
    CHECK(makeDeviceId(DeviceIdKind::Wasapi, "{abc}") == "wasapi:{abc}", "make wasapi id");
    // "default" must NEVER be prefixed: it is the spelling the picker and the
    // settings file already use for "let the system decide", and prefixing it
    // would make an existing selection unresolvable.
    CHECK(makeDeviceId(DeviceIdKind::Wasapi, "default") == "default",
          "\"default\" is never prefixed");
    CHECK(makeDeviceId(DeviceIdKind::Asio, "") == "default", "empty native -> default");

    for (const char *id : {"asio:{abc}", "wasapi:{abc}", "default"}) {
        const ParsedDeviceId p = parseDeviceId(id);
        CHECK(makeDeviceId(p.kind, p.native) == std::string(id), "make(parse(x)) == x");
    }
}

// --- 2. the buffer-size granularity walk ------------------------------------

static bool has(const std::vector<std::uint32_t> &v, std::uint32_t x)
{
    for (std::uint32_t e : v) if (e == x) return true;
    return false;
}

static void testBufferSizes()
{
    printf("\n-- asio_bufsize: the three granularity branches --\n");

    {
        // granularity 0 == FIXED. This is the REAL-HARDWARE case: measured on
        // the Tascam US-16x08, min == max == preferred == 256.
        const auto v = asioBufferSizeCandidates(256, 256, 256, 0);
        CHECK(v.size() == 1 && v[0] == 256, "granularity 0 -> exactly {preferred}");
    }
    {
        // A driver whose min != max but granularity is still 0 is saying the
        // same thing: only `preferred` is selectable.
        const auto v = asioBufferSizeCandidates(64, 2048, 512, 0);
        CHECK(v.size() == 1 && v[0] == 512, "granularity 0 with a range -> {preferred}");
    }
    {
        // granularity -1 == POWERS OF TWO.
        const auto v = asioBufferSizeCandidates(64, 1024, 256, -1);
        const std::vector<std::uint32_t> want{64, 128, 256, 512, 1024};
        CHECK(v == want, "granularity -1 -> powers of two, min..max");
    }
    {
        // A min that is not itself a power of two: start at the first one above.
        const auto v = asioBufferSizeCandidates(100, 1024, 256, -1);
        CHECK(!v.empty() && v.front() == 128, "granularity -1 rounds min UP to a power of two");
        CHECK(!has(v, 100u), "the non-power-of-two min is not offered");
    }
    {
        // granularity > 0 == an arithmetic series.
        const auto v = asioBufferSizeCandidates(64, 320, 128, 64);
        const std::vector<std::uint32_t> want{64, 128, 192, 256, 320};
        CHECK(v == want, "granularity 64 -> min, min+g, ... max");
    }
    {
        // `preferred` OUTSIDE the series must still be offerable: it is the one
        // value the driver guarantees works.
        const auto v = asioBufferSizeCandidates(64, 320, 100, 64);
        CHECK(has(v, 100u), "a preferred outside the series is added anyway");
        bool ascending = true;
        for (std::size_t i = 1; i < v.size(); ++i)
            if (v[i] <= v[i - 1]) ascending = false;
        CHECK(ascending, "the result is ascending and deduplicated");
    }
    {
        // A pathological driver must not produce a 100k-entry combo box, and
        // the window kept must be the one around `preferred`.
        const auto v = asioBufferSizeCandidates(1, 100000, 512, 1);
        CHECK(v.size() == kAsioMaxBufferChoices, "a huge series is capped");
        CHECK(has(v, 512u), "the cap keeps `preferred`");
    }
    {
        // Nonsense in, the one safe answer out — never an empty list, which
        // upstream would read as "not user-selectable" and lose the only size
        // known to work.
        const auto v = asioBufferSizeCandidates(0, 0, 512, 0);
        CHECK(v.size() == 1 && v[0] == 512, "bad min/max still yields {preferred}");
    }

    printf("\n-- asio_bufsize: snapping a request --\n");
    CHECK(asioSnapBufferSize(300, 64, 1024, 256, -1) == 256, "300 snaps to the nearer 256");
    CHECK(asioSnapBufferSize(400, 64, 1024, 256, -1) == 512, "400 snaps to the nearer 512");
    CHECK(asioSnapBufferSize(99999, 64, 1024, 256, -1) == 1024, "above max snaps to max");
    CHECK(asioSnapBufferSize(1, 64, 1024, 256, -1) == 64, "below min snaps to min");
    CHECK(asioSnapBufferSize(4096, 256, 256, 256, 0) == 256,
          "a FIXED driver snaps every request to its one size");
}

// --- 3. the sample converters ------------------------------------------------

static void testConverters()
{
    printf("\n-- asio_convert: round trip per type --\n");

    // Two channels interleaved, deliberately DIFFERENT per channel so a
    // converter that ignored `ch` or `stride` would show up.
    const std::size_t frames = 64;
    std::vector<float> src(frames * 2);
    for (std::size_t i = 0; i < frames; ++i) {
        src[i * 2 + 0] = std::sin((float) i * 0.11f);
        src[i * 2 + 1] = -std::cos((float) i * 0.07f) * 0.5f;
    }

    // THE INTEGER TOLERANCES ARE TWO LSB, NOT ONE, AND THAT IS DELIBERATE.
    // `asio_convert` reproduces `twConvertFrames`'s arithmetic exactly —
    // encode by 32767 with a clamp, decode by 32768, and TRUNCATE on the cast
    // rather than round to nearest (`twconvert.cc:31-34`). That costs up to one
    // LSB from the truncation plus one from the asymmetric scale, so a round
    // trip lands inside 2 LSB and not inside 1.
    //
    // Matching the house converter matters more than the last half-LSB: the
    // ASIO Int16 path and the WASAPI Int16 path must produce the SAME bytes
    // from the same float, or the two device routes would differ audibly-on-
    // paper for no reason anyone could find later. If the rounding is ever
    // improved, it must be improved in twconvert FIRST and here to match.
    struct TypeRow { AsioType t; float tol; };
    const TypeRow types[] = {
        {AsioType::Int16LSB,   2.0f / 32768.0f},
        {AsioType::Int24LSB,   2.0f / 8388608.0f},
        {AsioType::Int32LSB,   2.0f / 2147483648.0f * 1024.0f},  // float32 mantissa, not the type
        {AsioType::Float32LSB, 0.0f},
        {AsioType::Float64LSB, 0.0f},
    };

    for (const TypeRow &row : types) {
        std::vector<std::uint8_t> wire(frames * asioTypeBytes(row.t));
        std::vector<float>        back(frames * 2, 0.0f);

        for (std::size_t c = 0; c < 2; ++c) {
            asioFromFloat(wire.data(), row.t, src.data(), frames, 2, c);
            asioToFloat(back.data(), 2, c, wire.data(), row.t, frames);
        }

        float worst = 0.0f;
        for (std::size_t i = 0; i < frames * 2; ++i)
            worst = std::max(worst, std::fabs(back[i] - src[i]));

        char msg[160];
        std::snprintf(msg, sizeof(msg), "%s: de-interleaved round trip within %g (worst %g)",
                      asioTypeName(row.t), (double) row.tol, (double) worst);
        CHECK(worst <= row.tol, msg);
    }

    printf("\n-- asio_convert: the properties that bite --\n");
    {
        // PACKED Int24 is three bytes per sample, not a 32-bit container. Get
        // this wrong and every channel after the first is shifted.
        CHECK(asioTypeBytes(AsioType::Int24LSB) == 3, "Int24 occupies 3 bytes, not 4");
        std::vector<std::uint8_t> wire(4 * 3, 0xAA);
        std::vector<float> in{1.0f, 0.0f, -1.0f, 0.5f};
        asioFromFloat(wire.data(), AsioType::Int24LSB, in.data(), 4, 1, 0);
        // +1.0 -> 0x7FFFFF little-endian
        CHECK(wire[0] == 0xFF && wire[1] == 0xFF && wire[2] == 0x7F,
              "Int24 full scale is 0x7FFFFF, little-endian");
        CHECK(wire[3] == 0 && wire[4] == 0 && wire[5] == 0, "Int24 zero is three zero bytes");
    }
    {
        // Nothing upstream clips, so an out-of-range sample must SATURATE. If
        // it wrapped instead, a positive peak would become a full-scale
        // NEGATIVE one — the loudest possible failure.
        std::vector<float> hot{2.0f, -2.0f};
        std::vector<std::uint8_t> wire(2 * 2, 0);
        asioFromFloat(wire.data(), AsioType::Int16LSB, hot.data(), 2, 1, 0);
        const std::int16_t a = (std::int16_t) ((std::uint16_t) wire[0] | ((std::uint16_t) wire[1] << 8));
        const std::int16_t b = (std::int16_t) ((std::uint16_t) wire[2] | ((std::uint16_t) wire[3] << 8));
        CHECK(a == 32767, "+2.0 clamps to positive full scale, never wraps");
        CHECK(b == -32767, "-2.0 clamps to negative full scale");
    }
    {
        // The float types deliberately do NOT clamp: a float device path gets
        // exactly what the engine produced.
        std::vector<float> hot{2.0f};
        std::vector<std::uint8_t> wire(4, 0);
        asioFromFloat(wire.data(), AsioType::Float32LSB, hot.data(), 1, 1, 0);
        float outv = 0.0f;
        std::memcpy(&outv, wire.data(), 4);
        CHECK(outv == 2.0f, "Float32 passes an out-of-range sample through unclamped");
    }
    {
        CHECK(!asioTypeSupported(AsioType::Unsupported), "MSB/DSD map to Unsupported");
        CHECK(asioTypeBytes(AsioType::Unsupported) == 0, "an unsupported type has no size");
    }
}

// --- 4. the input channel set (Phase 3) --------------------------------------

static void testChannels()
{
    printf("\n-- asio_channels: mask -> opened set --\n");

    {
        const auto v = asioChannelsFromMask(0x1, 16);
        CHECK(v.size() == 1 && v[0] == 0, "bit 0 -> input 0 alone");
    }
    {
        const auto v = asioChannelsFromMask(0x21, 16);   // bits 0 and 5
        CHECK(v.size() == 2 && v[0] == 0 && v[1] == 5, "bits 0,5 -> inputs 0 and 5");
    }
    {
        // A project saved on a 16-input interface, opened on a 2-input one:
        // arm what exists rather than failing the open.
        const auto v = asioChannelsFromMask(0x21, 2);
        CHECK(v.size() == 1 && v[0] == 0, "bits above the device count are ignored");
    }
    CHECK(asioChannelsFromMask(0xFF, 0).empty(), "a device with no inputs opens none");

    printf("\n-- asio_channels: THE STREAM IS NOT COMPACTED --\n");

    // THE ONE THAT MATTERS. recordingChannels_ is a mask where bit n means
    // input n, applied per WAV sink. If opening {0,5} produced a 2-wide
    // stream, bit 5 would come to mean "the second channel I happened to
    // open" and a take would land on the wrong input, silently.
    CHECK(asioStreamWidthFor(0x21, 16) == 6, "opening {0,5} gives a 6-wide stream");
    CHECK(asioStreamWidthFor(0x1, 16) == 1, "opening {0} gives a 1-wide stream");
    CHECK(asioStreamWidthFor(0x8000, 16) == 16, "opening {15} alone still gives 16");
    CHECK(asioStreamWidthFor(0, 16) == 0, "opening nothing gives no stream");

    printf("\n-- asio_channels: grow-only --\n");

    std::uint64_t open = 0;
    open = asioGrowMask(open, 0x1, 16);
    CHECK(open == 0x1, "first arm opens input 0");
    open = asioGrowMask(open, 0x20, 16);
    CHECK(open == 0x21, "arming input 5 keeps input 0 open");
    open = asioGrowMask(open, 0x0, 16);
    CHECK(open == 0x21, "DISARMING EVERYTHING CLOSES NOTHING (grow-only)");
    open = asioGrowMask(open, 0xFFFF0000ull, 4);
    CHECK(open == 0x21, "bits above the device count never enter the set");

    printf("\n-- asio_channels: is a request already satisfied? --\n");

    // This is what decides whether a request may disturb a running stream:
    // satisfied -> do nothing; not satisfied -> defer to the next start.
    CHECK(asioMaskSatisfied(0x21, 0x1, 16), "an already-open channel needs no reopen");
    CHECK(asioMaskSatisfied(0x21, 0x21, 16), "the whole open set needs no reopen");
    CHECK(!asioMaskSatisfied(0x21, 0x2, 16), "a NEW channel does need one");
    CHECK(asioMaskSatisfied(0x1, 0x8000, 2),
          "a request the device cannot satisfy is not a reason to reopen");
    CHECK(asioMaskSatisfied(0, 0xFF, 0), "a device with no inputs is always satisfied");

    CHECK(kAsioDefaultInputMask == 1,
          "the default is input 0 alone, matching DEFAULT_RECORDING_CHANNELS");
}

int main()
{
    printf("=== multi_backend_test (proposal 35 Phases 2-3, cross-platform half) ===\n");
    testDeviceIds();
    testBufferSizes();
    testConverters();
    testChannels();

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
