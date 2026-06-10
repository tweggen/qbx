#ifndef _MP3_WRITER_H_
#define _MP3_WRITER_H_

#include <string>

#include "audio/audio_file_writer.h"

namespace audio {

class MP3Writer : public AudioFileWriter {
public:
    MP3Writer();
    ~MP3Writer() override;

    bool open(const std::string &path, const AudioFileConfig &config) override;
    bool write(const float *interleaved, std::size_t frameCount) override;
    bool close() override;

    const char *errorMessage() const override;

    static bool isAvailable();
    void setBitrate(int kbps);  // 128-320, default 192

private:
    bool initEncoder(const AudioFileConfig &config);

    void *lameHandle = nullptr;  // dlopen/LoadLibrary handle
    void *gfp = nullptr;         // lame_t (encoder state)

    std::string lastError;
    int bitrate = 192;
    std::uint32_t channels = 2;

    // Function pointers (loaded at runtime)
    void *(*fn_lame_init)() = nullptr;
    int (*fn_lame_set_in_samplerate)(void *, int) = nullptr;
    int (*fn_lame_set_num_channels)(void *, int) = nullptr;
    int (*fn_lame_set_out_samplerate)(void *, int) = nullptr;
    int (*fn_lame_set_brate)(void *, int) = nullptr;
    int (*fn_lame_set_quality)(void *, int) = nullptr;
    int (*fn_lame_init_params)(void *) = nullptr;
    int (*fn_lame_encode_buffer_ieee_float)(void *, const float *, const float *, int,
                                             unsigned char *, int) = nullptr;
    int (*fn_lame_encode_flush)(void *, unsigned char *, int) = nullptr;
    int (*fn_lame_close)(void *) = nullptr;

    bool loadLibrary();
};

}  // namespace audio

#endif
