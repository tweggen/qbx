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
