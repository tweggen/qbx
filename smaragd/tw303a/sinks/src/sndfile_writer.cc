#include "sndfile_writer.h"

#include "utf8_path.h"   // first: on Windows it brings <windows.h>, which
                         // sndfile.h needs for sf_wchar_open's prototype

#ifdef _WIN32
#define ENABLE_SNDFILE_WINDOWS_PROTOTYPES 1
#endif
#include <sndfile.h>

#include <algorithm>
#include <cstring>

#include "tw/core/twformat.h"

namespace audio {

namespace {

// The sample rates MPEG audio defines (MPEG-1, -2 and -2.5 Layer III). Any
// other rate fails inside libsndfile at open; checking first gives the user a
// sentence instead of an error code. The writer does NOT resample.
const std::uint32_t kMpegRates[] = { 8000, 11025, 12000, 16000, 22050,
                                     24000, 32000, 44100, 48000 };

const int kDefaultMp3Kbps = 192;

// sf_open with a UTF-8 path, on every platform. On Windows this build's
// libsndfile reads a narrow path in the ANSI code page (measured: an e-acute
// and a CJK ideograph came out as cp1252 mojibake on disk), so widen it and use
// sf_wchar_open. Nothing else differs: the handle, and so every byte written,
// is the same.
SNDFILE *openUtf8(const std::string &path, int mode, SF_INFO *info) {
#ifdef _WIN32
    const std::wstring wide = utf8ToWide(path);
    if (wide.empty()) return nullptr;
    return sf_wchar_open(wide.c_str(), mode, info);
#else
    return sf_open(path.c_str(), mode, info);
#endif
}

}  // namespace

std::string mp3ConfigError(std::uint32_t sampleRate, std::uint32_t channels) {
    if (channels < 1 || channels > 2) {
        return "MP3 carries one or two channels; this render has " +
               std::to_string(channels) +
               ". Render to WAV or OGG for more than two channels.";
    }
    if (std::find(std::begin(kMpegRates), std::end(kMpegRates), sampleRate) ==
        std::end(kMpegRates)) {
        return "MP3 has no " + std::to_string(sampleRate) +
               " Hz sample rate (it has 8000, 11025, 12000, 16000, 22050, "
               "24000, 32000, 44100 and 48000 Hz), and the render does not "
               "resample. Render to WAV or OGG at this rate.";
    }
    return std::string();
}

SndfileWriter::SndfileWriter(Container container) : container_(container) {}

SndfileWriter::~SndfileWriter() {
    if (sndFile) {
        close();
    }
}

int SndfileWriter::mp3KbpsForQuality(int quality) {
    // <= 10 is the OGG scale (0..10): SRenderAction's default is 10 and
    // RenderParams' is 6. Neither is a bitrate, so neither may become one.
    if (quality <= 10) {
        return kDefaultMp3Kbps;
    }
    // 128..320 is what the render dialog offers. Below 128 is a number
    // nobody asked for on purpose; above 320 MPEG-1 Layer III does not exist.
    return std::clamp(quality, 128, 320);
}

void SndfileWriter::setQuality(int quality) {
    // Applied at open(), so it must be called before. A WAV ignores it: the
    // file stays PCM 16 whatever the dialog's bit-depth box says (QBX-145
    // deliberately does not change WAV output).
    mp3Kbps_ = mp3KbpsForQuality(quality);
}

bool SndfileWriter::open(const std::string &path, const AudioFileConfig &config) {
    if (sndFile) {
        lastError = "File already open";
        return false;
    }

    if (container_ == Container::MP3) {
        return openMp3(path, config);
    }

    // WAV. Every call below is exactly what WAVWriter made, in the same order:
    // the WAV goldens are byte-compared and must not move.
    SF_INFO sfInfo = {};
    sfInfo.samplerate = static_cast<int>(config.sampleRate);
    sfInfo.channels = static_cast<int>(config.channels);
    sfInfo.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;

    sndFile = openUtf8(path, SFM_WRITE, &sfInfo);
    if (!sndFile) {
        lastError = std::string("Failed to open file for writing: ") + path + " (" +
                    sf_strerror(nullptr) + ")";
        return false;
    }

    // libsndfile does NOT clip float input to integer formats by default —
    // out-of-range samples wrap around (e.g. -1.9 becomes ~+0.1), turning a
    // hot mix into garbage. Enable saturating conversion instead.
    sf_command(static_cast<SNDFILE *>(sndFile), SFC_SET_CLIPPING, nullptr, SF_TRUE);

    return true;
}

bool SndfileWriter::openMp3(const std::string &path, const AudioFileConfig &config) {
    const std::string bad = mp3ConfigError(config.sampleRate, config.channels);
    if (!bad.empty()) {
        lastError = bad;
        return false;
    }

    SF_INFO sfInfo = {};
    sfInfo.samplerate = static_cast<int>(config.sampleRate);
    sfInfo.channels = static_cast<int>(config.channels);
    sfInfo.format = SF_FORMAT_MPEG | SF_FORMAT_MPEG_LAYER_III;

    SNDFILE *sf = openUtf8(path, SFM_WRITE, &sfInfo);
    if (!sf) {
        lastError = std::string("Failed to open MP3 file for writing: ") + path + " (" +
                    sf_strerror(nullptr) + ")";
        return false;
    }

    // NO sf_set_string, ever. Any string makes libsndfile write an ID3v2 tag,
    // and a bare sf_open of a tagged MP3 is the mpg123 hang on Windows that
    // tw/sources CONTRACT invariant 14 documents -- our own importer would hang
    // on our own export.

    // Bitrate. libsndfile's MPEG encoder defaults to VBR and takes a
    // "compression level" in [0, 1], which in CONSTANT mode at 32 kHz and above
    // it turns into `int bitrate = 320 - c * 288` (MPEG-2 rates use other
    // ranges; LAME snaps to the nearest bitrate the rate has). The half-kbps
    // nudge makes the int truncation land ON the asked bitrate instead of one
    // below it when c * 288 comes out a hair under an integer.
    //
    // ORDER MATTERS, and both must precede the first write: the level is
    // silently ignored once data has been written, and the mode then returns
    // SFE_CMD_HAS_DATA. Setting the mode re-applies the saved level, which is
    // why the level goes FIRST.
    double level = (320.0 - (double)mp3Kbps_ - 0.5) / 288.0;
    level = std::clamp(level, 0.0, 1.0);
    int mode = SF_BITRATE_MODE_CONSTANT;
    if (sf_command(sf, SFC_SET_COMPRESSION_LEVEL, &level, sizeof(level)) != SF_TRUE ||
        sf_command(sf, SFC_SET_BITRATE_MODE, &mode, sizeof(mode)) != SF_TRUE) {
        lastError = std::string("Failed to set the MP3 bitrate to ") +
                    std::to_string(mp3Kbps_) + " kbps (" + sf_strerror(sf) + ")";
        sf_close(sf);
        return false;
    }

    sndFile = sf;
    return true;
}

bool SndfileWriter::write(const float *interleaved, std::size_t frameCount) {
    if (!sndFile) {
        lastError = "File not open";
        return false;
    }

    sf_count_t written = sf_writef_float(static_cast<SNDFILE *>(sndFile), interleaved,
                                         static_cast<sf_count_t>(frameCount));

    if (written != static_cast<sf_count_t>(frameCount)) {
        lastError = std::string("Failed to write all frames to file (") +
                    sf_strerror(static_cast<SNDFILE *>(sndFile)) + ")";
        return false;
    }

    return true;
}

bool SndfileWriter::close() {
    if (!sndFile) {
        return true;
    }

    // For MP3 this is also where LAME is flushed and the LAME/Xing Info frame
    // (encoder delay and padding, so the file decodes gapless) is written.
    int result = sf_close(static_cast<SNDFILE *>(sndFile));
    sndFile = nullptr;

    if (result != 0) {
        lastError = std::string("Error closing audio file (") + sf_error_number(result) + ")";
        return false;
    }

    return true;
}

const char *SndfileWriter::errorMessage() const {
    return lastError.c_str();
}

// --- the availability probe ---------------------------------------------------

namespace {

// A virtual file that swallows everything: the probe needs libsndfile to open
// an MPEG encoder, not to produce a file.
struct DiscardFile {
    sf_count_t pos = 0;
    sf_count_t len = 0;
};

sf_count_t vioLength(void *u) { return static_cast<DiscardFile *>(u)->len; }

sf_count_t vioSeek(sf_count_t offset, int whence, void *u) {
    auto *f = static_cast<DiscardFile *>(u);
    switch (whence) {
        case SEEK_SET: f->pos = offset; break;
        case SEEK_CUR: f->pos += offset; break;
        case SEEK_END: f->pos = f->len + offset; break;
        default: break;
    }
    if (f->pos < 0) f->pos = 0;
    return f->pos;
}

sf_count_t vioRead(void *, sf_count_t, void *) { return 0; }

sf_count_t vioWrite(const void *, sf_count_t count, void *u) {
    auto *f = static_cast<DiscardFile *>(u);
    f->pos += count;
    if (f->pos > f->len) f->len = f->pos;
    return count;
}

sf_count_t vioTell(void *u) { return static_cast<DiscardFile *>(u)->pos; }

}  // namespace

bool mp3EncoderAvailable(std::uint32_t sampleRate, std::uint32_t channels,
                         std::string *reason) {
    const std::string bad = mp3ConfigError(sampleRate, channels);
    if (!bad.empty()) {
        if (reason) *reason = bad;
        return false;
    }

    // Ask libsndfile itself. sf_format_check() is NOT a probe: it accepts MPEG
    // with <= 2 channels whether or not the library was built with the encoder
    // (and whatever the rate). Opening an encoder is the only honest answer.
    SF_VIRTUAL_IO vio = {};
    vio.get_filelen = vioLength;
    vio.seek = vioSeek;
    vio.read = vioRead;
    vio.write = vioWrite;
    vio.tell = vioTell;
    DiscardFile sink;

    SF_INFO sfInfo = {};
    sfInfo.samplerate = static_cast<int>(sampleRate);
    sfInfo.channels = static_cast<int>(std::min<std::uint32_t>(channels, 2));
    sfInfo.format = SF_FORMAT_MPEG | SF_FORMAT_MPEG_LAYER_III;

    SNDFILE *sf = sf_open_virtual(&vio, SFM_WRITE, &sfInfo, &sink);
    if (!sf) {
        if (reason) {
            *reason = std::string("This build's libsndfile cannot encode MP3 (") +
                      sf_strerror(nullptr) + ")";
        }
        return false;
    }
    sf_close(sf);
    if (reason) reason->clear();
    return true;
}

}  // namespace audio
