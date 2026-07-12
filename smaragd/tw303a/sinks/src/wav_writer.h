#ifndef _WAV_WRITER_H_
#define _WAV_WRITER_H_

#include <string>

#include "tw/sinks/audio_file_writer.h"

struct SF_INFO;

namespace audio {

class WAVWriter : public AudioFileWriter {
public:
    WAVWriter();
    ~WAVWriter() override;

    bool open(const std::string &path, const AudioFileConfig &config) override;
    bool write(const float *interleaved, std::size_t frameCount) override;
    bool close() override;

    const char *errorMessage() const override;

private:
    void *sndFile = nullptr;
    std::string lastError;
};

}  // namespace audio

#endif
