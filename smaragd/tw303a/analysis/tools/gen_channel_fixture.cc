// gen_channel_fixture — write (and re-verify) the asymmetric multichannel fixture.
//
// The fixture is a WAV whose channels are DELIBERATELY unequal: channel c holds
// a sine at amplitude A0 / 2^c, so the per-channel RMS is a clean 6 dB ladder
// (0.5, 0.25, 0.125, 0.0625 for the committed 4-channel default). That is the
// only thing the file is for. Proposal 36 M0 needs a file where "which channel
// did you measure?" has a different answer per channel and every answer is known
// in advance — because until M0 the assert verbs SILENTLY IGNORED `channel=`
// whenever `frameCount` was omitted, and no fixture in the tree could tell.
//
// It exists as a COMMITTED tool, not a one-off script, for the same reason
// gen_position_fixture does: the fixture it writes is committed too, and a
// committed binary blob nobody can regenerate or check is a number no one may
// ever change. --verify measures each channel of an existing file and compares
// against the ladder, so the fixture is provable from source at any time.
//
// Deterministic by construction: no clock, no randomness, and an INTEGER number
// of cycles per channel over the file, so each channel's RMS is exactly
// amplitude/sqrt(2) rather than "about that, depending on where it was cut".
//
// Usage:
//   gen_channel_fixture --out <path> [--rate 48000] [--frames 12000]
//                       [--channels 4] [--hz 480] [--amp 0.70710678]
//   gen_channel_fixture --verify <path>
//
// Lives under analysis/tools/ so check_logging exempts its console output
// (tools/ is an ALLOW_DIR): reporting what it wrote, and which channel failed to
// verify, IS this program's job.

#include <sndfile.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

// M_PI is not in the C++ standard; MinGW hides it unless _USE_MATH_DEFINES is
// set before <cmath>, and this tool is not worth a build-system knob for one
// constant.
constexpr double kPi = 3.14159265358979323846;

// The committed fixture's shape. Kept here rather than in a header because
// nothing but this tool and the .qxa bands that quote these numbers cares.
constexpr int    kDefaultRate     = 48000;
constexpr int64_t kDefaultFrames  = 12000;        // 0.25 s at 48 kHz
constexpr int    kDefaultChannels = 4;
constexpr double kDefaultHz       = 480.0;        // 100 frames/cycle at 48 kHz
constexpr double kDefaultAmp      = 0.70710678118654752;  // ch 0 RMS = 0.5

double expectedRms(double amp0, int channel)
{
    // sin() over an integer number of cycles has mean square exactly 1/2.
    return (amp0 / std::pow(2.0, (double) channel)) / std::sqrt(2.0);
}

bool writeFixture(const std::string &path, int rate, int64_t frames,
                  int channels, double hz, double amp0, std::string &err)
{
    SF_INFO info;
    std::memset(&info, 0, sizeof(info));
    info.samplerate = rate;
    info.channels   = channels;
    info.format     = SF_FORMAT_WAV | SF_FORMAT_PCM_16;

    SNDFILE *f = sf_open(path.c_str(), SFM_WRITE, &info);
    if (!f) {
        err = std::string("write open failed: ") + sf_strerror(nullptr);
        return false;
    }

    const double w = 2.0 * kPi * hz / (double) rate;

    std::vector<double> inter((size_t)(frames * channels));
    for (int64_t t = 0; t < frames; ++t) {
        const double s = std::sin(w * (double) t);
        for (int c = 0; c < channels; ++c) {
            inter[(size_t)(t * channels + c)] =
                s * amp0 / std::pow(2.0, (double) c);
        }
    }

    if (sf_writef_double(f, inter.data(), frames) != frames) {
        err = std::string("short write: ") + sf_strerror(f);
        sf_close(f);
        return false;
    }
    sf_close(f);
    return true;
}

// Measure every channel of an existing file and compare against the ladder.
// This is what makes the committed fixture provable rather than merely present.
int verifyFixture(const std::string &path, double amp0)
{
    SF_INFO info;
    std::memset(&info, 0, sizeof(info));
    SNDFILE *f = sf_open(path.c_str(), SFM_READ, &info);
    if (!f) {
        std::printf("FAIL cannot open %s: %s\n", path.c_str(),
                    sf_strerror(nullptr));
        return 1;
    }

    std::printf("verify %s: %lld frames, %d Hz, %d ch\n", path.c_str(),
                (long long) info.frames, info.samplerate, info.channels);

    if (info.frames <= 0 || info.channels <= 0) {
        std::printf("FAIL file holds no audio\n");
        sf_close(f);
        return 1;
    }

    const int BUFFER = 4096;
    std::vector<double> buf((size_t)(BUFFER * info.channels));
    std::vector<double> sumSq((size_t) info.channels, 0.0);
    std::vector<double> peak((size_t) info.channels, 0.0);
    int64_t framesRead = 0;

    while (framesRead < info.frames) {
        const sf_count_t got = sf_readf_double(f, buf.data(), BUFFER);
        if (got <= 0) break;
        for (sf_count_t i = 0; i < got; ++i) {
            for (int c = 0; c < info.channels; ++c) {
                const double v = buf[(size_t)(i * info.channels + c)];
                sumSq[(size_t) c] += v * v;
                const double a = std::fabs(v);
                if (a > peak[(size_t) c]) peak[(size_t) c] = a;
            }
        }
        framesRead += got;
    }
    sf_close(f);

    // 16-bit quantisation costs ~1e-5; a 1e-3 tolerance is loose enough to be
    // stable and far tighter than the 6 dB spacing between neighbours.
    const double tol = 1.0e-3;
    int failures = 0;
    for (int c = 0; c < info.channels; ++c) {
        const double rms  = std::sqrt(sumSq[(size_t) c] / (double) framesRead);
        const double want = expectedRms(amp0, c);
        const bool ok = std::fabs(rms - want) <= tol;
        std::printf("  ch %d: rms %.6f (expected %.6f, %s), peak %.6f\n",
                    c, rms, want, ok ? "ok" : "MISMATCH", peak[(size_t) c]);
        if (!ok) ++failures;
    }

    if (failures) {
        std::printf("VERIFY FAILED (%d channel(s))\n", failures);
        return 1;
    }
    std::printf("VERIFY OK: %d channels on the expected 6 dB ladder\n",
                info.channels);
    return 0;
}

void usage()
{
    std::printf(
        "usage:\n"
        "  gen_channel_fixture --out <path> [--rate %d] [--frames %lld]\n"
        "                      [--channels %d] [--hz %.0f] [--amp %.8f]\n"
        "  gen_channel_fixture --verify <path> [--amp %.8f]\n",
        kDefaultRate, (long long) kDefaultFrames, kDefaultChannels,
        kDefaultHz, kDefaultAmp, kDefaultAmp);
}

}  // namespace

int main(int argc, char **argv)
{
    std::string outPath, verifyPath;
    int rate = kDefaultRate;
    int channels = kDefaultChannels;
    int64_t frames = kDefaultFrames;
    double hz = kDefaultHz;
    double amp0 = kDefaultAmp;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool hasNext = (i + 1 < argc);
        if (a == "--out" && hasNext)            outPath    = argv[++i];
        else if (a == "--verify" && hasNext)    verifyPath = argv[++i];
        else if (a == "--rate" && hasNext)      rate       = std::atoi(argv[++i]);
        else if (a == "--channels" && hasNext)  channels   = std::atoi(argv[++i]);
        else if (a == "--frames" && hasNext)    frames     = std::atoll(argv[++i]);
        else if (a == "--hz" && hasNext)        hz         = std::atof(argv[++i]);
        else if (a == "--amp" && hasNext)       amp0       = std::atof(argv[++i]);
        else if (a == "--help" || a == "-h")  { usage(); return 0; }
        else { std::printf("unknown argument: %s\n", a.c_str()); usage(); return 2; }
    }

    if (!verifyPath.empty()) {
        return verifyFixture(verifyPath, amp0);
    }
    if (outPath.empty()) {
        usage();
        return 2;
    }
    if (rate <= 0 || channels <= 0 || frames <= 0 || hz <= 0.0 || amp0 <= 0.0) {
        std::printf("rate, channels, frames, hz and amp must all be positive\n");
        return 2;
    }
    if (amp0 > 1.0) {
        std::printf("refusing: amplitude %.6f would clip\n", amp0);
        return 2;
    }

    // The per-channel RMS is only EXACTLY amplitude/sqrt(2) when the file holds
    // a whole number of cycles. Refuse a partial one rather than write a file
    // whose committed expectations would be off by an amount nobody can name.
    const double cycles = (double) frames * hz / (double) rate;
    if (std::fabs(cycles - std::round(cycles)) > 1.0e-9) {
        std::printf("refusing: %lld frames at %.3f Hz / %d Hz is %.6f cycles, "
                    "not a whole number - the per-channel RMS would not be "
                    "exactly amp/sqrt(2)\n",
                    (long long) frames, hz, rate, cycles);
        return 2;
    }

    // The quietest channel must stay well clear of the 16-bit noise floor
    // (~1.5e-5), or its committed RMS band would be measuring quantisation.
    const double quietest = amp0 / std::pow(2.0, (double)(channels - 1));
    if (quietest < 1.0e-3) {
        std::printf("refusing: channel %d would peak at %.8f, too close to the "
                    "16-bit noise floor\n", channels - 1, quietest);
        return 2;
    }

    std::string err;
    if (!writeFixture(outPath, rate, frames, channels, hz, amp0, err)) {
        std::printf("FAILED: %s\n", err.c_str());
        return 1;
    }

    std::printf("wrote %s: %lld frames (%.3f s), %d Hz, %d ch, %.3f Hz tone, "
                "%.0f cycles\n",
                outPath.c_str(), (long long) frames,
                (double) frames / (double) rate, rate, channels, hz, cycles);
    for (int c = 0; c < channels; ++c) {
        std::printf("  ch %d: peak %.8f, rms %.8f\n",
                    c, amp0 / std::pow(2.0, (double) c), expectedRms(amp0, c));
    }
    return 0;
}
