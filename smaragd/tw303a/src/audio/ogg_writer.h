#ifndef _OGG_WRITER_H_
#define _OGG_WRITER_H_

#include <string>
#include <vector>

#include "audio/audio_file_writer.h"

struct ogg_stream_state;
struct ogg_page;
struct ogg_packet;
struct vorbis_info;
struct vorbis_comment;
struct vorbis_dsp_state;
struct vorbis_block;

namespace audio {

class OGGWriter : public AudioFileWriter {
public:
    OGGWriter();
    ~OGGWriter() override;

    bool open(const std::string &path, const AudioFileConfig &config) override;
    bool write(const float *interleaved, std::size_t frameCount) override;
    bool close() override;

    const char *errorMessage() const override;

    void setQuality(int quality);  // 0-10, default 6

private:
    bool writePages();
    bool flushPages(bool eos);

    std::FILE *outFile = nullptr;
    ogg_stream_state *streamState = nullptr;
    vorbis_info *vorbisInfo = nullptr;
    vorbis_comment *vorbisComment = nullptr;
    vorbis_dsp_state *vorbisState = nullptr;
    vorbis_block *vorbisBlock = nullptr;

    int quality = 6;
    std::uint32_t channels = 2;
    bool headerWritten = false;
    std::string lastError;
};

}  // namespace audio

#endif
