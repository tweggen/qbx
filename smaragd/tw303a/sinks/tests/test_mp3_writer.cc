// test_mp3_writer.cc — MP3 export writes a real, decodable MP3 (QBX-145).
//
// MP3 export never worked. The old MP3Writer dlopen'd libmp3lame and its
// open() returned false unconditionally ("MP3 writing not yet fully
// implemented"), so every MP3 render failed -- and the only test tw_sinks had
// was a STRING test over the dlopen candidate list, which passed throughout.
// This replaces it with the property that matters: a sine written through
// createAudioFileWriter(MP3) reads back through libsndfile (mpg123) with the
// right length and the right energy.
//
// It also pins the writer's refusals -- more than two channels and a sample
// rate MPEG audio does not have -- because both used to be impossible to reach
// (open failed first), and the availability probe the render dialog asks must
// AGREE with them, or the dialog offers a format the writer then refuses.

#include "tw/sinks/audio_file_writer.h"

#include <sndfile.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

static int failures = 0;
#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (cond) { std::printf("ok   %s\n", msg); }                        \
        else      { std::printf("FAIL %s\n", msg); ++failures; }            \
    } while (0)

static const double kPi = 3.14159265358979323846;

// An interleaved sine, `amp` peak, on every channel.
static std::vector<float> sine(std::size_t frames, unsigned channels,
                               double rate, double hz, float amp)
{
    std::vector<float> v(frames * channels);
    for (std::size_t i = 0; i < frames; ++i) {
        const float s = amp * (float)std::sin(2.0 * kPi * hz * (double)i / rate);
        for (unsigned c = 0; c < channels; ++c) v[i * channels + c] = s;
    }
    return v;
}

// Writes `frames` of a sine through the factory's MP3 writer. `kbps <= 0`
// leaves the writer's default. Returns false (and prints why) on any failure.
static bool writeMp3(const std::string &path, std::uint32_t rate,
                     unsigned channels, std::size_t frames, int kbps,
                     std::string *error = nullptr)
{
    auto w = audio::createAudioFileWriter(audio::AudioFormat::MP3);
    if (!w) return false;
    if (kbps > 0) w->setQuality(kbps);
    audio::AudioFileConfig cfg;
    cfg.sampleRate = rate;
    cfg.channels = channels;
    if (!w->open(path, cfg)) {
        if (error) *error = w->errorMessage();
        std::printf("     open: %s\n", w->errorMessage());
        return false;
    }
    const std::vector<float> s = sine(frames, channels, (double)rate, 440.0, 0.5f);
    // Several writes, as FileSink makes them, not one.
    const std::size_t block = 2048;
    for (std::size_t off = 0; off < frames; off += block) {
        const std::size_t n = std::min(block, frames - off);
        if (!w->write(s.data() + off * channels, n)) {
            std::printf("     write: %s\n", w->errorMessage());
            return false;
        }
    }
    if (!w->close()) {
        std::printf("     close: %s\n", w->errorMessage());
        return false;
    }
    return true;
}

static long fileSize(const std::string &path)
{
    std::FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return -1;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fclose(f);
    return n;
}

int main()
{
    const std::uint32_t rate = 48000;
    const unsigned ch = 2;
    const std::size_t frames = 72000;   // 1.5 s
    const std::string path = "mp3_writer_test.mp3";

    // --- 1. a stereo sine round-trips ---------------------------------------
    std::printf("\n-- write and read back --\n");
    const bool wrote = writeMp3(path, rate, ch, frames, 192);
    CHECK(wrote, "MP3 writer opens, writes and closes a stereo sine");

    SF_INFO info = {};
    SNDFILE *in = wrote ? sf_open(path.c_str(), SFM_READ, &info) : nullptr;
    CHECK(in != nullptr, "libsndfile opens the MP3 for reading");
    if (in) {
        std::printf("     read back: %lld frames, %d ch, %d Hz (wrote %zu)\n",
                    (long long)info.frames, info.channels, info.samplerate, frames);
        CHECK(info.channels == (int)ch && info.samplerate == (int)rate,
              "channels and rate survive the round trip");
        // The LAME/Xing Info frame sf_close writes carries the encoder delay
        // and padding, so mpg123 reads back gapless -- exact, or within one
        // MPEG-1 Layer III frame (1152 samples) where it is not.
        const long long diff = (long long)info.frames - (long long)frames;
        CHECK(diff >= -1152 && diff <= 1152,
              "frame count within one MP3 frame of what was written");

        std::vector<float> back((std::size_t)info.frames * (std::size_t)info.channels);
        const sf_count_t got = sf_readf_float(in, back.data(), info.frames);
        sf_close(in);
        // RMS over the middle, away from the codec's edges.
        double acc = 0.0;
        std::size_t n = 0;
        for (sf_count_t i = 4096; i + 4096 < got; ++i) {
            for (int c = 0; c < info.channels; ++c) {
                const double v = back[(std::size_t)i * info.channels + c];
                acc += v * v;
                ++n;
            }
        }
        const double rms = n ? std::sqrt(acc / (double)n) : 0.0;
        const double want = 0.5 / std::sqrt(2.0);
        std::printf("     RMS %.4f, expected %.4f\n", rms, want);
        CHECK(std::fabs(rms - want) < 0.05 * want, "RMS within 5% of the sine's");
    }

    // --- 2. refusals ---------------------------------------------------------
    std::printf("\n-- refusals --\n");
    std::string why;
    CHECK(!writeMp3("mp3_writer_6ch.mp3", rate, 6, 4800, 192, &why) && !why.empty(),
          "six channels are refused, with a message");
    why.clear();
    CHECK(!writeMp3("mp3_writer_96k.mp3", 96000, 2, 4800, 192, &why) && !why.empty(),
          "96 kHz is refused, with a message");

    // --- 3. the probe agrees with the writer --------------------------------
    std::printf("\n-- availability probe --\n");
    std::string reason;
    CHECK(audio::mp3EncoderAvailable(48000, 2, &reason), "probe: 48 kHz stereo is available");
    CHECK(audio::mp3EncoderAvailable(44100, 1, &reason), "probe: 44.1 kHz mono is available");
    reason.clear();
    CHECK(!audio::mp3EncoderAvailable(96000, 2, &reason) && !reason.empty(),
          "probe: 96 kHz is unavailable, with a reason");
    std::printf("     reason: %s\n", reason.c_str());
    reason.clear();
    CHECK(!audio::mp3EncoderAvailable(48000, 6, &reason) && !reason.empty(),
          "probe: six channels are unavailable, with a reason");
    std::printf("     reason: %s\n", reason.c_str());

    // --- 4. the bitrate reaches the encoder ---------------------------------
    // CBR, so the size is (nearly) proportional to the bitrate. Before
    // QBX-145 the quality never reached any writer.
    std::printf("\n-- bitrate --\n");
    const bool lo = writeMp3("mp3_writer_128.mp3", rate, ch, frames, 128);
    const bool hi = writeMp3("mp3_writer_320.mp3", rate, ch, frames, 320);
    const long s128 = fileSize("mp3_writer_128.mp3");
    const long s320 = fileSize("mp3_writer_320.mp3");
    std::printf("     128 kbps: %ld bytes, 320 kbps: %ld bytes\n", s128, s320);
    CHECK(lo && hi && s320 > 2 * s128, "320 kbps is well over twice the size of 128 kbps");
    // 1.5 s at 192 kbps CBR is 36000 bytes of audio, plus the Info frame.
    const long s192 = fileSize(path);
    std::printf("     192 kbps: %ld bytes\n", s192);
    CHECK(s192 > 34000 && s192 < 40000, "192 kbps CBR has the size 192 kbps implies");
    // A legacy quality value (the render action's default is 10, an OGG-scale
    // number) means "the default bitrate", not 10 kbps clamped to something.
    CHECK(writeMp3("mp3_writer_q10.mp3", rate, ch, frames, 10) &&
              fileSize("mp3_writer_q10.mp3") == s192,
          "quality 10 encodes exactly as 192 kbps");

#ifdef _WIN32
    // --- 5. a non-ANSI path --------------------------------------------------
    // The dialog hands the writers UTF-8 (QString::toStdString). libsndfile
    // treats it as UTF-8; OGGWriter used std::fopen, which reads it in the
    // ANSI code page and creates a mojibake name instead.
    std::printf("\n-- UTF-8 paths --\n");
    const char *u8 = "sinks_\xC3\xA9\xE6\x97\xA5";   // sinks_ + e-acute + a CJK ideograph
    const wchar_t *wide = L"sinks_\u00E9\u65E5";
    struct { audio::AudioFormat fmt; const char *ext; const wchar_t *wext; const char *name; } fmts[] = {
        { audio::AudioFormat::WAV, ".wav", L".wav", "WAV opens a UTF-8 path as UTF-8" },
        { audio::AudioFormat::OGG, ".ogg", L".ogg", "OGG opens a UTF-8 path as UTF-8" },
        { audio::AudioFormat::MP3, ".mp3", L".mp3", "MP3 opens a UTF-8 path as UTF-8" },
    };
    for (auto &f : fmts) {
        const std::string p = std::string(u8) + f.ext;
        const std::wstring wp = std::wstring(wide) + f.wext;
        DeleteFileW(wp.c_str());
        auto w = audio::createAudioFileWriter(f.fmt);
        audio::AudioFileConfig cfg;
        cfg.sampleRate = rate;
        cfg.channels = ch;
        bool ok = w->open(p, cfg);
        if (ok) {
            const std::vector<float> s = sine(4800, ch, (double)rate, 440.0, 0.5f);
            ok = w->write(s.data(), 4800) && w->close();
        } else {
            std::printf("     open: %s\n", w->errorMessage());
        }
        const DWORD attr = GetFileAttributesW(wp.c_str());
        CHECK(ok && attr != INVALID_FILE_ATTRIBUTES, f.name);
        DeleteFileW(wp.c_str());
    }
#endif

    std::printf("\n%s\n\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
