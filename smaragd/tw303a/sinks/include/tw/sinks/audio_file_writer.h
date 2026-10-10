#ifndef _AUDIO_FILE_WRITER_H_
#define _AUDIO_FILE_WRITER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "tw/core/twformat.h"

namespace audio {

struct AudioFileConfig {
    std::uint32_t sampleRate = 48000;
    std::uint32_t channels = 2;
    twSampleType sampleType = twSampleType::Float32;
};

enum class AudioFormat { WAV, OGG, MP3 };

// MP3 export loads libmp3lame AT RUNTIME rather than linking it, so whether it
// works is a deployment question answered at run time, not a build flag.
//
// These two are declared HERE, in the public header, because the app has to ask
// and MP3Writer lives in sinks/src/ where the app layer cannot include it.
// srenderdialog.cpp previously hand-wrote its own `class MP3Writer { static
// bool isAvailable(); }` to get at it -- a duplicate declaration that linked
// only because the member was static, and that would silently rot the moment
// the real class changed.
bool mp3WriterAvailable();

// The library names that were tried, comma-separated, so a failure can say what
// it looked for instead of telling the user to copy a file the build may
// already have deployed under a different name.
std::string mp3WriterCandidates();

class AudioFileWriter {
public:
    virtual ~AudioFileWriter() = default;

    virtual bool open(const std::string &path, const AudioFileConfig &config) = 0;
    virtual bool write(const float *interleaved, std::size_t frameCount) = 0;
    virtual bool close() = 0;

    virtual const char *errorMessage() const = 0;
};

std::unique_ptr<AudioFileWriter> createAudioFileWriter(AudioFormat format);

}  // namespace audio

#endif
