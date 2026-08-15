// gen_position_fixture — write (and re-verify) the position-coded test fixture.
//
// The fixture is the "integer-cycle tone staircase" of tw/core/position_code.h:
// audio whose CONTENT names its own absolute source frame, so a test can ask
// "which part of the source is this?" of a rendered WAV and get a number back.
// See that header for the encoding and why the cycle sequence is clamped.
//
// It exists as a COMMITTED tool, not a one-off script, because the fixture it
// writes is committed too. test_sawtooth.wav has no generator: nobody can
// regenerate it, prove it is what it claims, or change it without changing
// every expectation keyed to it by hand. This tool's --verify mode decodes
// every block of an existing file and fails on the first mismatch, so the
// committed fixture is provable at any time and reproducible from source.
//
// Deterministic by construction: no clock, no randomness, integer cycle
// arithmetic, one code path.
//
// Usage:
//   gen_position_fixture --out <path> [--rate 48000] [--blocks 187]
//                        [--channels 1]
//   gen_position_fixture --verify <path>
//
// Lives under analysis/tools/ so check_logging exempts its console output
// (tools/ is an ALLOW_DIR): reporting what it wrote, and which block failed to
// verify, IS this program's job.

#include "tw/core/position_code.h"

#include <sndfile.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace poscode = tw::poscode;

namespace {

// Write `blocks` encoded blocks as 16-bit PCM.
//
// 16-bit, matching what the render path produces: if the encoding did not
// survive quantisation there would be no point in a decoder that reads renders.
// It does, with room to spare — a 0.5-amplitude tone quantised to 16 bits sits
// ~90 dB over the noise floor, while the decode margin only needs the winning
// bin to beat bins the signal is mathematically orthogonal to.
bool writeFixture(const std::string &path, int rate, int64_t blocks,
                  int channels, std::string &err)
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

    std::vector<double> inter((size_t)(poscode::kBlockFrames * channels));
    int cycles = poscode::kFirstCycles;
    for (int64_t k = 0; k < blocks; ++k) {
        for (int64_t t = 0; t < poscode::kBlockFrames; ++t) {
            const double v = poscode::sampleInBlock(cycles, t);
            for (int c = 0; c < channels; ++c)
                inter[(size_t)(t * channels + c)] = v;
        }
        if (sf_writef_double(f, inter.data(), poscode::kBlockFrames)
                != poscode::kBlockFrames) {
            err = std::string("short write: ") + sf_strerror(f);
            sf_close(f);
            return false;
        }
        cycles = poscode::nextCycles(cycles);
    }
    sf_close(f);
    return true;
}

// Decode every block of an existing file and compare against the encoding.
// This is what makes the committed fixture provable rather than merely present.
int verifyFixture(const std::string &path)
{
    SF_INFO info;
    std::memset(&info, 0, sizeof(info));
    SNDFILE *f = sf_open(path.c_str(), SFM_READ, &info);
    if (!f) {
        std::printf("FAIL cannot open %s: %s\n", path.c_str(),
                    sf_strerror(nullptr));
        return 1;
    }

    const int64_t blocks = info.frames / poscode::kBlockFrames;
    std::printf("verify %s: %lld frames, %d Hz, %d ch -> %lld blocks\n",
                path.c_str(), (long long)info.frames, info.samplerate,
                info.channels, (long long)blocks);

    if (blocks <= 0) {
        std::printf("FAIL file holds no complete block\n");
        sf_close(f);
        return 1;
    }
    if (info.frames % poscode::kBlockFrames != 0) {
        std::printf("FAIL frame count %lld is not a multiple of the %lld-frame "
                    "block\n", (long long)info.frames,
                    (long long)poscode::kBlockFrames);
        sf_close(f);
        return 1;
    }

    std::vector<double> inter((size_t)(poscode::kBlockFrames * info.channels));
    std::vector<double> mono((size_t)poscode::kBlockFrames);

    int failures = 0;
    double worstConfidence = 1.0e30;
    int64_t worstBlock = -1;

    for (int64_t k = 0; k < blocks; ++k) {
        const sf_count_t got = sf_readf_double(f, inter.data(),
                                               poscode::kBlockFrames);
        if (got != poscode::kBlockFrames) {
            std::printf("FAIL block %lld: short read (%lld frames)\n",
                        (long long)k, (long long)got);
            ++failures;
            break;
        }
        for (int64_t t = 0; t < poscode::kBlockFrames; ++t) {
            double v = 0.0;
            for (int c = 0; c < info.channels; ++c)
                v += inter[(size_t)(t * info.channels + c)];
            mono[(size_t)t] = v / (double)info.channels;
        }

        const poscode::Decode d =
            poscode::decodeBuffer(mono.data(), poscode::kBlockFrames, blocks);

        if (d.confidence < worstConfidence) {
            worstConfidence = d.confidence;
            worstBlock = k;
        }
        if (d.silent || d.blockIndex != k) {
            std::printf("FAIL block %lld decoded as %lld (frame %lld), "
                        "confidence %.1f, rms %.4f\n",
                        (long long)k, (long long)d.blockIndex,
                        (long long)d.sourceFrame, d.confidence, d.rms);
            if (++failures >= 10) {
                std::printf("... stopping after 10 failures\n");
                break;
            }
        }
    }
    sf_close(f);

    if (failures) {
        std::printf("VERIFY FAILED (%d block(s))\n", failures);
        return 1;
    }
    std::printf("first block: %d cycles = %.2f Hz\n",
                poscode::cyclesForBlock(0),
                poscode::frequencyForBlock(0, info.samplerate));
    std::printf("last  block: %d cycles = %.2f Hz (Nyquist bin %lld)\n",
                poscode::cyclesForBlock(blocks - 1),
                poscode::frequencyForBlock(blocks - 1, info.samplerate),
                (long long)(poscode::kBlockFrames / 2));
    std::printf("weakest decode: block %lld at confidence %.1f\n",
                (long long)worstBlock, worstConfidence);
    std::printf("VERIFY OK: %lld/%lld blocks decode to their own position\n",
                (long long)blocks, (long long)blocks);
    return 0;
}

void usage()
{
    std::printf(
        "usage:\n"
        "  gen_position_fixture --out <path> [--rate 48000] [--blocks %d] "
        "[--channels 1]\n"
        "  gen_position_fixture --verify <path>\n",
        poscode::kDefaultBlocks);
}

}  // namespace

int main(int argc, char **argv)
{
    std::string outPath, verifyPath;
    int rate = 48000;
    int channels = 1;
    int64_t blocks = poscode::kDefaultBlocks;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool hasNext = (i + 1 < argc);
        if (a == "--out" && hasNext)            outPath    = argv[++i];
        else if (a == "--verify" && hasNext)    verifyPath = argv[++i];
        else if (a == "--rate" && hasNext)      rate       = std::atoi(argv[++i]);
        else if (a == "--channels" && hasNext)  channels   = std::atoi(argv[++i]);
        else if (a == "--blocks" && hasNext)    blocks     = std::atoll(argv[++i]);
        else if (a == "--help" || a == "-h")  { usage(); return 0; }
        else { std::printf("unknown argument: %s\n", a.c_str()); usage(); return 2; }
    }

    if (!verifyPath.empty()) {
        return verifyFixture(verifyPath);
    }
    if (outPath.empty()) {
        usage();
        return 2;
    }
    if (rate <= 0 || channels <= 0 || blocks <= 0) {
        std::printf("rate, channels and blocks must all be positive\n");
        return 2;
    }

    // The encoding is only valid while the top bin stays below Nyquist. The
    // clamp in position_code.h keeps that true for far more blocks than anyone
    // will ask for, but a caller raising --blocks past it should be stopped
    // here rather than handed an aliased file that decodes as nonsense.
    const int topCycles = poscode::maxCyclesForBlocks(blocks);
    if (topCycles >= poscode::kBlockFrames / 2) {
        std::printf("refusing: %lld blocks would reach bin %d, at or past the "
                    "Nyquist bin %lld\n", (long long)blocks, topCycles,
                    (long long)(poscode::kBlockFrames / 2));
        return 2;
    }

    std::string err;
    if (!writeFixture(outPath, rate, blocks, channels, err)) {
        std::printf("FAILED: %s\n", err.c_str());
        return 1;
    }

    std::printf("wrote %s: %lld blocks, %lld frames (%.2f s), %d Hz, %d ch\n",
                outPath.c_str(), (long long)blocks,
                (long long)(blocks * poscode::kBlockFrames),
                (double)(blocks * poscode::kBlockFrames) / (double)rate,
                rate, channels);
    std::printf("block 0: %d cycles = %.2f Hz; block %lld: %d cycles = %.2f Hz\n",
                poscode::cyclesForBlock(0),
                poscode::frequencyForBlock(0, rate),
                (long long)(blocks - 1), topCycles,
                poscode::frequencyForBlock(blocks - 1, rate));
    return 0;
}
