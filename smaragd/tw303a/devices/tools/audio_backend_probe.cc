// audio_backend_probe — drive the PRODUCTION output path against a real
// device. Proposal 35, Phase 2. A target, not a gate (like clap_probe and
// vst3_probe).
//
// `asio_probe` proves the ABI by talking to IASIO directly. This one proves the
// BACKEND, through exactly the objects the app uses: `createAudioBackend()` ->
// `WinMultiBackend` -> `AsioBackend`/`WASAPIBackend`. Nothing here reaches
// around them, so what it exercises is what `twSpeaker` will get:
//
//   list                 the MERGED device list, with the ids as persisted
//   open  <id>           open + report config/rates/buffer sizes, then close
//   tone  <id> [seconds] the whole lifecycle with a 440 Hz sine through the
//                        real RenderCallback seam
//
// Run it on BOTH an `asio:` id and a bare WASAPI one: the second is the
// dispatcher regression that matters most, because a bare/legacy id must still
// reach WASAPI unchanged (see asio_id.h).
//
// There is no headless-suite version of this and there cannot be: every path
// it walks needs a real endpoint or a real driver.

#include "tw/devices/audio_backend.h"

#ifdef TW_HAVE_ASIO
// PRIVATE to the module, and reached here on purpose: the gate's own
// counter is the only direct evidence of whether this driver delivers a
// callback after ASIOStop() returns. acquire() with an empty name returns
// the LIVE instance, so this queries the device the backend already holds.
#  include "asio_device.h"
#endif

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#  include <combaseapi.h>
#endif

namespace {

const char *fmtName(twSampleType t)
{
    switch (t) {
    case twSampleType::Float32: return "Float32";
    case twSampleType::Int16:   return "Int16";
    case twSampleType::Int32:   return "Int32";
    default:                    return "?";
    }
}

int cmdList()
{
    auto be = audio::createAudioBackend();
    if (!be) { std::printf("createAudioBackend() returned nothing\n"); return 1; }

    std::printf("backend: %s\n", be->name());
    const std::vector<audio::AudioDeviceInfo> devs = be->enumerateDevices();
    if (devs.empty()) {
        std::printf("  (no enumeration; only the system default is selectable)\n");
        return 0;
    }
    for (const audio::AudioDeviceInfo &d : devs) {
        std::printf("  %-52s  %s", d.id.c_str(), d.name.c_str());
        if (d.sampleRate) std::printf("  [%u Hz]", d.sampleRate);
        std::printf("\n");
    }
    std::printf("%zu device(s)\n", devs.size());
    return 0;
}

int openAndReport(audio::AudioBackend *be, const std::string &id, std::uint32_t rate)
{
    if (be->openDevice(id, rate) != 0) {
        std::printf("  FAILED: openDevice('%s') — see the log lines above\n", id.c_str());
        return 1;
    }
    // The dispatcher reports the ACTIVE backend, which is the thing worth
    // seeing: it says which of the two actually took the id.
    std::printf("  routed : %s\n", be->name());

    const audio::AudioConfig c = be->getConfig();
    std::printf("  config : %u Hz, %u ch, %u frames/buffer, %s, out latency %u frames\n",
                c.sampleRate, c.channels, c.bufferFrames, fmtName(c.sampleType),
                c.outputLatencyFrames);

    std::printf("  rates  :");
    for (std::uint32_t r : be->supportedRates()) std::printf(" %u", r);
    std::printf("\n");

    const std::vector<std::uint32_t> sizes = be->getAvailableBufferSizes();
    std::printf("  buffers:");
    if (sizes.empty()) std::printf(" (not user-selectable)");
    for (std::uint32_t s : sizes) std::printf(" %u", s);
    std::printf("\n");

    // THE CHANNEL COUNT IS THE PHASE 2 DECISION MADE VISIBLE. An ASIO driver
    // with 8 outputs must report 2 here — we open outputs 1-2 only, so the
    // monitor mix cannot land on a headphone amp or an outboard send.
    if (c.channels > 2)
        std::printf("  ! more than two output channels reported — the two-output policy "
                    "is not in force\n");
    return 0;
}

int cmdOpen(const std::string &id, std::uint32_t rate)
{
    auto be = audio::createAudioBackend();
    if (!be) return 1;
    std::printf("=== open %s\n", id.c_str());
    const int rc = openAndReport(be.get(), id, rate);
    be->closeDevice();
    if (rc == 0) std::printf("  RESULT : open lifecycle OK\n");
    return rc;
}

int cmdTone(const std::string &id, int seconds, std::uint32_t rate)
{
    auto be = audio::createAudioBackend();
    if (!be) return 1;

    std::printf("=== tone %s (%d s)\n", id.c_str(), seconds);
    if (openAndReport(be.get(), id, rate) != 0) return 1;

    const audio::AudioConfig c = be->getConfig();
    const double step = 2.0 * 3.14159265358979323846 * 440.0 / (double) c.sampleRate;

    // The callback runs on the DRIVER's thread. Everything it touches is
    // captured by value or is an atomic; nothing here allocates or logs.
    //
    // IT ALSO TIMES ITSELF, because "the tone had a click in it" is not a
    // diagnosis. A dropout shows up here as one inter-callback GAP far larger
    // than the buffer period; a phase discontinuity in our own generator would
    // not. Recording the worst gap AND WHERE IT FELL separates the two without
    // anyone having to listen twice.
    double phase = 0.0;
    std::uint64_t framesSeen = 0;
    std::uint64_t calls = 0;
    double worstGapMs = 0.0;
    double worstGapAtS = 0.0;
    double worstWorkMs = 0.0;
    std::uint64_t bigGaps = 0;
    double firstCallbackMs = -1.0;
    // Measured from AFTER startOutput() RETURNS, which is the only reading
    // that can be compared with asio_probe: both sleep for `seconds` starting
    // there, so it is the window the frame count is judged against.
    double firstAfterStartMs = -1.0;
    std::chrono::steady_clock::time_point tStarted{};
    bool started = false;
    const auto t0 = std::chrono::steady_clock::now();
    auto prev = t0;

    be->setRenderCallback(
        [&](float *out, std::size_t frames, std::uint32_t ch) {
            const auto now = std::chrono::steady_clock::now();
            const double sinceStartMs =
                std::chrono::duration<double, std::milli>(now - t0).count();
            if (firstCallbackMs < 0.0) {
                firstCallbackMs = sinceStartMs;
                if (started)
                    firstAfterStartMs =
                        std::chrono::duration<double, std::milli>(now - tStarted).count();
            } else {
                const double gapMs =
                    std::chrono::duration<double, std::milli>(now - prev).count();
                if (gapMs > worstGapMs) {
                    worstGapMs  = gapMs;
                    worstGapAtS = sinceStartMs / 1000.0;
                }
                if (gapMs > 2.0 * 1000.0 * (double) frames / 48000.0) ++bigGaps;
            }
            prev = now;
            ++calls;

            for (std::size_t i = 0; i < frames; ++i, phase += step) {
                const float v = (float) (std::sin(phase) * 0.25);
                for (std::uint32_t k = 0; k < ch; ++k) out[i * ch + k] = v;
            }
            framesSeen += frames;

            // HOW LONG WE TOOK, which is the half of the question the gap
            // cannot answer. A gap of 3 buffer periods means either the host
            // held the driver up or the driver never called; only the work
            // time separates the two, and it is the only one of the two we
            // could fix.
            const double workMs =
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - now).count();
            if (workMs > worstWorkMs) worstWorkMs = workMs;
            return frames;
        });

    // TIME THE START ITSELF. ASIOStart may block while the driver primes, and
    // a blocking start is indistinguishable from a slow ramp unless the two
    // are measured apart.
    const auto tBeforeStart = std::chrono::steady_clock::now();
    if (be->startOutput() != 0) {
        std::printf("  FAILED: startOutput\n");
        be->closeDevice();
        return 1;
    }
    tStarted = std::chrono::steady_clock::now();
    started  = true;
    const double startCallMs =
        std::chrono::duration<double, std::milli>(tStarted - tBeforeStart).count();
    const double openToStartMs =
        std::chrono::duration<double, std::milli>(tBeforeStart - t0).count();
    std::printf("  running: you should HEAR a 440 Hz sine for %d s\n", seconds);
    std::this_thread::sleep_for(std::chrono::seconds(seconds));

    const std::uint64_t before = framesSeen;
    be->stopOutput();

    // THE SAMPLE MUST BE TAKEN AFTER stopOutput() RETURNS, and getting this
    // wrong reads as a fence failure that is not one. The contract is "no
    // callback runs after stopOutput RETURNS" — one already in flight when the
    // stop was requested is entitled to finish, and the fence exists precisely
    // to wait for it. Comparing against a count sampled BEFORE the call reports
    // every ordinary stop as a violation, because the in-flight callback
    // increments between the two reads. Measured: that false positive fired on
    // 2 runs in 3, at exactly one buffer (256 frames) — which is what an
    // in-flight callback looks like.
    const std::uint64_t atReturn = framesSeen;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const std::uint64_t after = framesSeen;

    const std::uint64_t expect = (std::uint64_t) c.sampleRate * (std::uint64_t) seconds;
    std::printf("  frames : %llu delivered, ~%llu expected (%.0f%%)\n",
                (unsigned long long) before, (unsigned long long) expect,
                expect ? 100.0 * (double) before / (double) expect : 0.0);

    // The period one callback SHOULD take, against what happened. A gap far
    // above the period is a dropout, and a dropout is what a click sounds
    // like; the start-up delay is reported separately so it cannot be
    // mistaken for one.
    const double periodMs = c.bufferFrames && c.sampleRate
                                ? 1000.0 * (double) c.bufferFrames / (double) c.sampleRate
                                : 0.0;
    std::printf("  timing : %llu callbacks, period %.2f ms; first at %.1f ms; "
                "worst gap %.2f ms at t=%.2f s; %llu gap(s) > 2x period; "
                "worst OUR work %.3f ms\n",
                (unsigned long long) calls, periodMs, firstCallbackMs, worstGapMs,
                worstGapAtS, (unsigned long long) bigGaps, worstWorkMs);
    if (periodMs > 0.0 && worstGapMs > periodMs * 2.0) {
        std::printf("  ! a gap of %.1fx the buffer period — a dropout, i.e. a click\n",
                    worstGapMs / periodMs);
        // Attribute it. If our own work never came close to the period, the
        // host did not cause the gap and no change here can remove it.
        if (worstWorkMs < periodMs * 0.5)
            std::printf("           our worst callback took %.3f ms of a %.2f ms "
                        "period (%.1f%%), so the gap is the DRIVER or the OS, "
                        "not the host\n",
                        worstWorkMs, periodMs, 100.0 * worstWorkMs / periodMs);
        else
            std::printf("           our worst callback took %.3f ms of a %.2f ms "
                        "period — the HOST is implicated\n",
                        worstWorkMs, periodMs);
    }
    std::printf("  startup: setRenderCallback+prep %.1f ms; startOutput() call itself "
                "%.1f ms; first callback %.1f ms AFTER start returned\n",
                openToStartMs, startCallMs, firstAfterStartMs);
    if (firstCallbackMs > 50.0)
        std::printf("  note   : %.0f ms total before the first callback, and the "
                    "sleep window begins after start() returns\n",
                    firstCallbackMs);

    int rc = 0;
    if (before != atReturn)
        std::printf("  note   : %llu frame(s) from a callback in flight at the stop — "
                    "expected, and what the fence waits for\n",
                    (unsigned long long) (atReturn - before));
    if (after != atReturn) {
        std::printf("  ! %llu frames arrived AFTER stopOutput() returned — the render "
                    "callback was reached after the gate closed\n",
                    (unsigned long long) (after - atReturn));
        rc = 1;
    }
    if (before < expect / 2) {
        std::printf("  ! far fewer frames than the clock implies — the stream did not run\n");
        rc = 1;
    }

#ifdef TW_HAVE_ASIO
    if (id.rfind("asio:", 0) == 0 || id.rfind("ASIO:", 0) == 0) {
        if (auto dev = audio::AsioDevice::acquire("", 0, nullptr))
            std::printf("  gate   : %llu callback(s) arrived after the gate closed "
                        "and were turned away\n",
                        (unsigned long long) dev->lateCallbacks());
    }
#endif
    be->closeDevice();
    std::printf("  RESULT : %s\n", rc == 0 ? "tone lifecycle OK" : "FAILED");
    return rc;
}

}  // namespace

int main(int argc, char **argv)
{
#if defined(_WIN32)
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    // ASIO drivers are in-proc COM servers; WASAPI needs COM too. The app does
    // this inside the backend, but a probe has to own its own apartment.
    CoInitialize(nullptr);
#endif

    std::vector<std::string> av;
    std::uint32_t rate = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a.rfind("--rate=", 0) == 0) rate = (std::uint32_t) std::atoi(a.c_str() + 7);
        else av.push_back(a);
    }

    int rc = 2;
    if (av.empty()) {
        std::printf("usage: audio_backend_probe list\n"
                    "       audio_backend_probe open <device-id> [--rate=N]\n"
                    "       audio_backend_probe tone <device-id> [seconds] [--rate=N]\n"
                    "\n"
                    "Drives the PRODUCTION output path (createAudioBackend -> the Windows\n"
                    "dispatcher -> WASAPI or ASIO). Device ids come from `list`; a BARE id\n"
                    "and \"default\" route to WASAPI, an `asio:` id to the ASIO backend.\n");
    } else if (av[0] == "list") {
        rc = cmdList();
    } else if (av[0] == "open" && av.size() >= 2) {
        rc = cmdOpen(av[1], rate);
    } else if (av[0] == "tone" && av.size() >= 2) {
        int seconds = 2;
        if (av.size() >= 3) {
            seconds = std::atoi(av[2].c_str());
            if (seconds < 1 || seconds > 60) seconds = 2;
        }
        rc = cmdTone(av[1], seconds, rate);
    } else {
        std::printf("unknown or incomplete command '%s'\n", av[0].c_str());
    }

#if defined(_WIN32)
    CoUninitialize();
#endif
    return rc;
}
