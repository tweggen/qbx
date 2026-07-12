#include "wav_writer.h"

#include <sndfile.h>

#include "tw/core/twformat.h"

namespace audio {

WAVWriter::WAVWriter() : sndFile(nullptr) {}

WAVWriter::~WAVWriter() {
    if (sndFile) {
        close();
    }
}

bool WAVWriter::open(const std::string &path, const AudioFileConfig &config) {
    if (sndFile) {
        lastError = "File already open";
        return false;
    }

    SF_INFO sfInfo = {};
    sfInfo.samplerate = static_cast<int>(config.sampleRate);
    sfInfo.channels = static_cast<int>(config.channels);
    sfInfo.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;

    sndFile = sf_open(path.c_str(), SFM_WRITE, &sfInfo);
    if (!sndFile) {
        lastError = std::string("Failed to open file for writing: ") + path;
        return false;
    }

    // libsndfile does NOT clip float input to integer formats by default —
    // out-of-range samples wrap around (e.g. -1.9 becomes ~+0.1), turning a
    // hot mix into garbage. Enable saturating conversion instead.
    sf_command(static_cast<SNDFILE *>(sndFile), SFC_SET_CLIPPING, nullptr, SF_TRUE);

    return true;
}

bool WAVWriter::write(const float *interleaved, std::size_t frameCount) {
    if (!sndFile) {
        lastError = "File not open";
        return false;
    }

    sf_count_t written = sf_writef_float(static_cast<SNDFILE *>(sndFile), interleaved,
                                         static_cast<sf_count_t>(frameCount));

    if (written != static_cast<sf_count_t>(frameCount)) {
        lastError = "Failed to write all frames to file";
        return false;
    }

    return true;
}

bool WAVWriter::close() {
    if (!sndFile) {
        return true;
    }

    int result = sf_close(static_cast<SNDFILE *>(sndFile));
    sndFile = nullptr;

    if (result != 0) {
        lastError = "Error closing audio file";
        return false;
    }

    return true;
}

const char *WAVWriter::errorMessage() const {
    return lastError.c_str();
}

}  // namespace audio
