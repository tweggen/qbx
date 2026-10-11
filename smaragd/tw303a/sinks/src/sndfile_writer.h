#ifndef _SNDFILE_WRITER_H_
#define _SNDFILE_WRITER_H_

#include <string>

#include "tw/sinks/audio_file_writer.h"

namespace audio {

// Every format libsndfile writes for us, behind one open/write/close (QBX-145).
//
// It was WAVWriter until MP3 export moved onto libsndfile too. The two formats
// differ ONLY in what open() asks for and how it configures the handle before
// the first write; writing frames and closing are the same calls.
//
//   WAV  SF_FORMAT_WAV | SF_FORMAT_PCM_16, SFC_SET_CLIPPING. Byte-identical to
//        what WAVWriter wrote -- every WAV golden in the suite depends on it.
//   MP3  SF_FORMAT_MPEG | SF_FORMAT_MPEG_LAYER_III, constant bitrate. libsndfile
//        drives LAME itself (its `mpeg` feature: mp3lame + mpg123), so there is
//        no library to find at run time any more -- the format is compiled into
//        the libsndfile the build links, or it is not, and
//        mp3EncoderAvailable() asks which.
class SndfileWriter : public AudioFileWriter {
public:
    enum class Container { WAV, MP3 };

    explicit SndfileWriter(Container container);
    ~SndfileWriter() override;

    bool open(const std::string &path, const AudioFileConfig &config) override;
    bool write(const float *interleaved, std::size_t frameCount) override;
    bool close() override;

    const char *errorMessage() const override;

    // MP3: kbps (see RenderParams::quality). WAV: ignored.
    void setQuality(int quality) override;

    // The kbps an MP3 render will actually use for `quality`. Exposed for the
    // mapping's own documentation and for tests: values <= 10 are the OGG-scale
    // numbers the render action and RenderParams default to, and mean "the
    // default bitrate" (192), not ten kilobits clamped to something.
    static int mp3KbpsForQuality(int quality);

private:
    bool openMp3(const std::string &path, const AudioFileConfig &config);

    Container container_;
    void *sndFile = nullptr;
    int mp3Kbps_ = 192;
    std::string lastError;
};

// Why MP3 cannot be written at this rate/width, or "" if it can as far as the
// container goes (1-2 channels, an MPEG sample rate). Shared by the writer's
// open() and the availability probe so the two can never disagree.
std::string mp3ConfigError(std::uint32_t sampleRate, std::uint32_t channels);

}  // namespace audio

#endif
