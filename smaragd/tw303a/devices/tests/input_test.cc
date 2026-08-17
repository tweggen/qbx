// tw/devices INPUT gate (proposal 21 L0, AC1).
//
// Four things, none of which had a gate before:
//
//   (a) FileAudioInput replays a 2 s POSITION-CODED WAV through a real capture
//       thread and a real ring: every frame exactly once, in order, byte-equal
//       to the file, and each 1024-frame block available within a stated
//       tolerance of its paced due time. The pacing is the point — the file
//       backend exists so a live/monitor/recording case runs against a device
//       that behaves like a device (design D9), and a replay that handed the
//       whole file over at once would remove exactly the pressure such a case
//       is looking for.
//
//   (b) THE TAIL-DROP REGRESSION. Design §1 F7: WASAPIInput::read() copied
//       min(packet, caller's buffer) and then released the WHOLE packet, so
//       every frame past the caller's buffer was lost and the recorded
//       timeline silently compressed. The fix is structural — a capture thread
//       pushes whole packets into an SPSC ring and read() pops — so the gate is
//       on the ring: a producer writing a packet THREE TIMES the consumer's pop
//       size must lose nothing.
//
//   (c) The capture MIDI input's inject() delivers in order, with the stamps it
//       was given (0 == now), which is what the L0 testkit verbs sit on.
//
//   (d) SMARAGD_AUDIO_INPUT_BACKEND selection, including `null` — the value a
//       --test-case run defaults to so a headless suite never opens the
//       developer's microphone.
//
// Like devices_midi_test this measures WALL-CLOCK behaviour, so it is
// RUN_SERIAL and it measures the MACHINE as much as the code: confirm the box
// is idle before reading a timing failure as a regression.

#include "tw/devices/audio_input.h"
#include "tw/devices/audio_ring.h"
#include "tw/devices/capture_midi.h"
#include "tw/devices/midi_out_scheduler.h"
#include "tw/core/position_code.h"

#include "file_input.h"          // PRIVATE to the module (src/), see CMakeLists
#include "precise_waiter.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (cond) { std::printf("ok   %s\n", msg); }                        \
        else      { std::printf("FAIL %s\n", msg); ++failures; }            \
    } while (0)

namespace poscode = tw::poscode;

namespace {

// Write a 16-bit PCM mono WAV holding `blocks` position-coded blocks.
//
// Generated rather than committed: the encoding lives in tw/core and both sides
// of this test come from it, so there is no third artifact to keep in step. Two
// seconds at 48 kHz is 23.4 blocks, so 24 blocks (98304 frames, 2.048 s) is the
// smallest whole-block fixture that covers the two seconds AC1a asks for.
bool writePositionWav(const std::string &path, int rate, int blocks)
{
    const std::size_t frames = (std::size_t)(blocks * poscode::kBlockFrames);
    std::vector<std::int16_t> pcm(frames);

    int cycles = poscode::kFirstCycles;
    std::size_t o = 0;
    for (int k = 0; k < blocks; ++k) {
        for (std::int64_t t = 0; t < poscode::kBlockFrames; ++t) {
            const double v = poscode::sampleInBlock(cycles, t);
            int s = (int)(v * 32767.0 + (v >= 0 ? 0.5 : -0.5));
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            pcm[o++] = (std::int16_t)s;
        }
        cycles = poscode::nextCycles(cycles);
    }

    const std::uint32_t dataBytes = (std::uint32_t)(pcm.size() * 2);
    unsigned char h[44];
    std::memcpy(h, "RIFF", 4);
    const std::uint32_t riffSize = 36 + dataBytes;
    std::memcpy(h + 4, &riffSize, 4);
    std::memcpy(h + 8, "WAVEfmt ", 8);
    const std::uint32_t fmtSize = 16;   std::memcpy(h + 16, &fmtSize, 4);
    const std::uint16_t fmt = 1;        std::memcpy(h + 20, &fmt, 2);
    const std::uint16_t ch = 1;         std::memcpy(h + 22, &ch, 2);
    const std::uint32_t sr = (std::uint32_t)rate;  std::memcpy(h + 24, &sr, 4);
    const std::uint32_t bps = sr * 2;   std::memcpy(h + 28, &bps, 4);
    const std::uint16_t align = 2;      std::memcpy(h + 32, &align, 2);
    const std::uint16_t bits = 16;      std::memcpy(h + 34, &bits, 2);
    std::memcpy(h + 36, "data", 4);     std::memcpy(h + 40, &dataBytes, 4);

    std::FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(h, 1, 44, f) == 44 &&
                    std::fwrite(pcm.data(), 2, pcm.size(), f) == pcm.size();
    std::fclose(f);
    return ok;
}

void setEnv(const char *name, const char *value)
{
#if defined(_WIN32)
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1); else unsetenv(name);
#endif
}

}  // namespace

int main()
{
    // ---- (b) the ring: a packet 3x the pop size loses nothing --------------
    //
    // The WASAPI tail-drop regression, at the level the fix actually lives.
    {
        audio::AudioRing ring;
        ring.reset(2, 8192);

        const std::size_t popSize = 1024;
        const std::size_t packet = popSize * 3;          // one WASAPI packet
        std::vector<float> in(packet * 2);
        for (std::size_t f = 0; f < packet; ++f) {
            in[f * 2 + 0] = (float)f;
            in[f * 2 + 1] = (float)f + 0.5f;
        }

        const std::size_t pushed = ring.push(in.data(), packet);
        CHECK(pushed == packet, "the whole 3x packet is accepted by the ring");
        CHECK(ring.overrunFrames() == 0, "a packet that fits counts no overrun");

        bool ordered = true;
        std::vector<float> out(popSize * 2);
        for (int i = 0; i < 3; ++i) {
            const std::size_t got = ring.pop(out.data(), popSize);
            if (got != popSize) { ordered = false; break; }
            for (std::size_t f = 0; f < popSize; ++f) {
                const float want = (float)(i * (int)popSize + (int)f);
                if (out[f * 2] != want || out[f * 2 + 1] != want + 0.5f) {
                    ordered = false;
                    break;
                }
            }
            if (!ordered) break;
        }
        CHECK(ordered,
              "all 3072 frames come back, in order, across three 1024 pops "
              "(the WASAPI tail-drop regression)");
        CHECK(ring.availableFrames() == 0 && ring.framesPopped() == packet,
              "the ring is empty afterwards and the counters agree");

        // And the overrun policy: what does not fit is COUNTED, never silently
        // written over frames a consumer may be reading.
        audio::AudioRing small;
        small.reset(1, 1024);                 // rounds to 1024
        std::vector<float> big(4096, 1.0f);
        const std::size_t took = small.push(big.data(), 4096);
        CHECK(took == 1024 && small.overrunFrames() == 4096 - 1024,
              "an over-large push takes what fits and counts the rest as overrun");

        // A SHORT pop is an underrun; an EMPTY one is not (an idle device that
        // hands over nothing is the normal case, not a hole).
        audio::AudioRing halfFull;
        halfFull.reset(1, 1024);
        std::vector<float> half(100, 0.5f);
        halfFull.push(half.data(), 100);
        std::vector<float> want(256, 0.0f);
        const std::size_t got1 = halfFull.pop(want.data(), 256);
        const std::size_t got2 = halfFull.pop(want.data(), 256);
        CHECK(got1 == 100 && halfFull.underrunFrames() == 156 && got2 == 0 &&
                  halfFull.underrunFrames() == 156,
              "a short pop counts an underrun; an empty pop does not");
    }

    // ---- (d) backend selection --------------------------------------------
    {
        setEnv("SMARAGD_AUDIO_INPUT_BACKEND", "");
        auto def = audio::createAudioInput();
        CHECK(def != nullptr, "an unset selector yields the platform input");
        const std::string platform = def ? def->backendName() : "";

        setEnv("SMARAGD_AUDIO_INPUT_BACKEND", "null");
        auto n = audio::createAudioInput();
        CHECK(n && std::string(n->backendName()) == "null",
              "SMARAGD_AUDIO_INPUT_BACKEND=null selects the null input "
              "(the --test-case default: a headless suite opens no microphone)");

        // The null input is inert in the way the default depends on: it opens
        // without touching hardware and reads silence.
        std::vector<float> buf(256 * 2, 1.0f);
        CHECK(n && n->openDevice("default", 48000) == 0 &&
                  n->startCapture() == 0 &&
                  n->read(buf.data(), 256) == 256 && buf[0] == 0.0f,
              "the null input opens, captures and reads silence");

        setEnv("SMARAGD_AUDIO_INPUT_BACKEND", "default");
        auto d2 = audio::createAudioInput();
        CHECK(d2 && std::string(d2->backendName()) == platform,
              "`default` is the same platform pick as an unset selector");

        setEnv("SMARAGD_AUDIO_INPUT_BACKEND", "banana");
        auto bad = audio::createAudioInput();
        CHECK(bad && std::string(bad->backendName()) == platform,
              "an unknown selector warns and falls back to the platform, "
              "never to a null pointer");

        setEnv("SMARAGD_AUDIO_INPUT_BACKEND", "file:");
        auto noPath = audio::createAudioInput();
        CHECK(noPath && std::string(noPath->backendName()) == platform,
              "`file:` with no path falls back rather than half-working");

        setEnv("SMARAGD_AUDIO_INPUT_BACKEND", "");
    }

    // ---- (a) FileAudioInput: every frame once, in order, on schedule -------
    const std::string wav = "l0_position_2s.wav";
    const int rate = 48000;
    const int blocks = 24;                      // 98304 frames = 2.048 s
    CHECK(writePositionWav(wav, rate, blocks),
          "wrote the 2 s position-coded fixture");

    {
        // Through the FACTORY, so the file selector and the companion knobs are
        // exercised on the same object the pacing is measured on.
        setEnv("SMARAGD_AUDIO_INPUT_LATENCY_FRAMES", "512");
        setEnv("SMARAGD_AUDIO_INPUT_LOOP", "0");
        auto owned = audio::createAudioInput("file:" + wav);
        auto *in = dynamic_cast<audio::FileAudioInput *>(owned.get());
        CHECK(in != nullptr && std::string(owned->backendName()) == "file",
              "file:<path> selects FileAudioInput");
        setEnv("SMARAGD_AUDIO_INPUT_LATENCY_FRAMES", "");
        setEnv("SMARAGD_AUDIO_INPUT_LOOP", "");

        if (!in) { std::printf("FAIL no file input; skipping (a)\n"); ++failures; }
        else {
            CHECK(in->openDevice("default", (std::uint32_t)rate) == 0,
                  "the file input opens the fixture");
            CHECK(in->getConfig().sampleRate == (std::uint32_t)rate &&
                      in->getConfig().channels == 1 &&
                      in->fileFrames() ==
                          (std::size_t)(blocks * poscode::kBlockFrames),
                  "rate, channels and length come from the file");
            CHECK(in->getLatencyFrames() == 512,
                  "the reported input latency is the configured one "
                  "(SMARAGD_AUDIO_INPUT_LATENCY_FRAMES)");
            CHECK(!in->loop(), "SMARAGD_AUDIO_INPUT_LOOP=0 stops at end");

            // Prove the fixture really is position-coded before asserting
            // anything about the frames that come out of it.
            {
                const auto &s = in->fileSamples();
                std::vector<double> w((std::size_t)poscode::kBlockFrames);
                bool decoded = true;
                for (int k : {0, 7, blocks - 1}) {
                    for (std::int64_t t = 0; t < poscode::kBlockFrames; ++t)
                        w[(std::size_t)t] =
                            s[(std::size_t)(k * poscode::kBlockFrames + t)];
                    const poscode::Decode d =
                        poscode::decodeBuffer(w.data(), poscode::kBlockFrames,
                                              blocks);
                    if (d.silent || d.blockIndex != k) decoded = false;
                }
                CHECK(decoded,
                      "the fixture decodes to its own positions (it is a "
                      "position code, not just a tone)");
            }

            const std::size_t total = in->fileFrames();
            const std::size_t blockFrames = audio::FileAudioInput::kBlockFrames;
            const std::int64_t period =
                (std::int64_t)((double)blockFrames * 1e9 / (double)rate);

            std::vector<float> got;
            got.reserve(total);
            std::vector<std::int64_t> arrival;      // per delivered block
            std::vector<float> chunk(blockFrames);

            audio::PreciseWaiter poller;
            poller.open();

            CHECK(in->startCapture() == 0, "capture starts");
            const std::int64_t t0 = in->captureStartHostNs();

            // Poll finely. std::this_thread::sleep_for on Windows rounds up to
            // the system tick, which would put 15 ms of quantisation on a 21 ms
            // block — hence the same high-resolution timer the producer uses.
            const std::int64_t deadline =
                audio::MidiOutScheduler::hostNowNs() + 10'000'000'000LL;
            while (got.size() < total &&
                   audio::MidiOutScheduler::hostNowNs() < deadline) {
                const std::int32_t n =
                    in->read(chunk.data(), blockFrames);
                if (n > 0) {
                    const std::int64_t now = audio::MidiOutScheduler::hostNowNs();
                    for (std::int32_t i = 0; i < n; ++i) got.push_back(chunk[i]);
                    // Stamp every completed block boundary this read crossed.
                    while (arrival.size() < got.size() / blockFrames)
                        arrival.push_back(now);
                } else {
                    poller.waitUntil(audio::MidiOutScheduler::hostNowNs() +
                                     200'000);   // 0.2 ms
                }
            }
            poller.close();

            CHECK(got.size() == total,
                  "every frame of the file was delivered");

            bool exact = got.size() == total;
            std::size_t firstBad = 0;
            const auto &src = in->fileSamples();
            for (std::size_t i = 0; i < got.size() && i < total; ++i) {
                if (got[i] != src[i]) { exact = false; firstBad = i; break; }
            }
            if (!exact && got.size() == total)
                std::printf("     first mismatch at frame %zu\n", firstBad);
            CHECK(exact,
                  "the delivered frames are the file's, in order, sample-exact "
                  "(nothing duplicated, nothing dropped)");

            const audio::AudioInputStats st = in->stats();
            CHECK(st.framesPushed == total && st.framesPopped == total &&
                      st.overrunFrames == 0,
                  "the ring counters agree with the file and count no overrun");

            // THE PACING, measured twice, because two different things are
            // being asked about.
            //
            //  1. THE DEVICE'S SCHEDULE - when the capture thread actually
            //     handed each block over, from its own block log (the file
            //     backend's analogue of CaptureBackend's {hostTimeNs,
            //     firstFrame} log, devices inv. 9). Block i is due at
            //     t0 + (i+1)*period. This is the property the backend
            //     PROMISES, and the one the 2 ms bound is on.
            //
            //  2. WHAT A CONSUMER SAW - when a polling reader could first
            //     observe those frames. That number also contains the ring
            //     hop, the reader's 0.2 ms poll quantum and the reader
            //     thread's own scheduling, so it is REPORTED and bounded
            //     loosely rather than pinned: a desktop deschedules an
            //     ordinary thread for a few milliseconds now and then, and
            //     asserting tightly on it would gate the machine rather than
            //     the code. Measured at 3.4 ms once in five runs taken right
            //     after a full ctest sweep, while the device-side number stayed
            //     inside 2 ms - which is exactly the distinction.
            std::vector<std::int64_t> pushed;
            const std::size_t nPushed = in->blockLog(pushed);
            std::int64_t maxErr = 0;
            std::size_t worst = 0;
            for (std::size_t i = 0; i < nPushed; ++i) {
                const std::int64_t due = t0 + (std::int64_t)(i + 1) * period;
                const std::int64_t err = pushed[i] - due;
                const std::int64_t a = err < 0 ? -err : err;
                if (a > maxErr) { maxErr = a; worst = i; }
            }
            std::int64_t maxSeen = 0;
            std::size_t worstSeen = 0;
            for (std::size_t i = 0; i < arrival.size(); ++i) {
                const std::int64_t due = t0 + (std::int64_t)(i + 1) * period;
                const std::int64_t err = arrival[i] - due;
                const std::int64_t a = err < 0 ? -err : err;
                if (a > maxSeen) { maxSeen = a; worstSeen = i; }
            }
            std::printf("     paced delivery: %zu blocks, period %.3f ms; "
                        "max |pushed - due| = %.3f ms (block %zu); "
                        "max |consumer saw - due| = %.3f ms (block %zu)\n",
                        nPushed, (double)period / 1e6,
                        (double)maxErr / 1e6, worst,
                        (double)maxSeen / 1e6, worstSeen);
            CHECK(nPushed == arrival.size(),
                  "the device logged exactly the blocks the consumer received");
            CHECK(maxErr <= 2000000,
                  "every block was DELIVERED within 2 ms of its paced due time");
            CHECK(maxSeen <= 15000000,
                  "and a polling consumer saw each one within 15 ms of it "
                  "(the ring hop plus the reader's own scheduling)");

            CHECK(in->stopCapture() == 0, "capture stops (thread joined)");
            CHECK(in->closeDevice() == 0, "device closes");
        }
    }
    std::remove(wav.c_str());

    // ---- (c) injected MIDI comes out in order, with its stamps -------------
    {
        audio::CaptureMidiInput midi;

        struct Rec { std::vector<std::uint8_t> bytes; std::int64_t host; };
        std::vector<Rec> seen;
        midi.setCallback([&seen](const std::uint8_t *b, std::size_t n,
                                 std::int64_t host) {
            seen.push_back({ std::vector<std::uint8_t>(b, b + n), host });
        });
        CHECK(midi.open("capture") == 0, "the capture MIDI input opens");

        const std::int64_t base = audio::MidiOutScheduler::hostNowNs();
        for (int i = 0; i < 8; ++i) {
            const std::uint8_t msg[3] = { 0x90, (std::uint8_t)(60 + i), 100 };
            midi.inject(msg, 3, base + (std::int64_t)i * 1'000'000);
        }
        // 0 == now: the stamp must be filled in, not left at zero.
        const std::uint8_t cc[3] = { 0xB0, 7, 64 };
        midi.inject(cc, 3, 0);

        bool ordered = seen.size() == 9;
        for (std::size_t i = 0; ordered && i < 8; ++i) {
            ordered = seen[i].bytes.size() == 3 &&
                      seen[i].bytes[0] == 0x90 &&
                      seen[i].bytes[1] == (std::uint8_t)(60 + i) &&
                      seen[i].host == base + (std::int64_t)i * 1'000'000;
        }
        CHECK(ordered,
              "injected events reach the callback in order, with the stamps "
              "they were given");
        CHECK(seen.size() == 9 && seen[8].bytes[0] == 0xB0 &&
                  seen[8].host >= base,
              "hostTimeNs == 0 is stamped with now, not left at zero");
        CHECK(audio::CaptureMidiInput::active() == &midi,
              "the live capture input is reachable statically (the testkit's "
              "only handle on it)");
        midi.close();
    }

    std::printf("%s (%d failure(s))\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
