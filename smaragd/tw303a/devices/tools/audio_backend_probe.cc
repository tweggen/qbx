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
    double phase = 0.0;
    std::uint64_t framesSeen = 0;
    be->setRenderCallback(
        [&phase, &framesSeen, step](float *out, std::size_t frames, std::uint32_t ch) {
            for (std::size_t i = 0; i < frames; ++i, phase += step) {
                const float v = (float) (std::sin(phase) * 0.25);
                for (std::uint32_t k = 0; k < ch; ++k) out[i * ch + k] = v;
            }
            framesSeen += frames;
            return frames;
        });

    if (be->startOutput() != 0) {
        std::printf("  FAILED: startOutput\n");
        be->closeDevice();
        return 1;
    }
    std::printf("  running: you should HEAR a 440 Hz sine for %d s\n", seconds);
    std::this_thread::sleep_for(std::chrono::seconds(seconds));

    const std::uint64_t before = framesSeen;
    be->stopOutput();
    // After stopOutput() returns, the contract is "no callback in flight or
    // forthcoming". On ASIO there is no thread to join — the fence in
    // AsioDevice is what has to hold — so this is where a violation shows up.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const std::uint64_t after = framesSeen;

    const std::uint64_t expect = (std::uint64_t) c.sampleRate * (std::uint64_t) seconds;
    std::printf("  frames : %llu delivered, ~%llu expected (%.0f%%)\n",
                (unsigned long long) before, (unsigned long long) expect,
                expect ? 100.0 * (double) before / (double) expect : 0.0);

    int rc = 0;
    if (after != before) {
        std::printf("  ! %llu frames arrived AFTER stopOutput() returned — the stop fence "
                    "did not hold\n",
                    (unsigned long long) (after - before));
        rc = 1;
    }
    if (before < expect / 2) {
        std::printf("  ! far fewer frames than the clock implies — the stream did not run\n");
        rc = 1;
    }

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
