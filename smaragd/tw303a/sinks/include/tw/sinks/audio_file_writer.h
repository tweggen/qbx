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

// Whether an MP3 of this sample rate and width can be written (QBX-145).
//
// MP3 is written by libsndfile (SF_FORMAT_MPEG | SF_FORMAT_MPEG_LAYER_III, LAME
// inside), the library the WAV writer uses. It used to be a separate writer
// that dlopen'd libmp3lame at run time and whose open() always failed, so MP3
// export had never worked. Whether it works is now a question about the
// libsndfile this build links -- its `mpeg` feature, the same one MP3 IMPORT
// needs -- plus two facts about the format itself: one or two channels, and one
// of the nine MPEG sample rates. The writer does not resample or fold channels.
//
// The probe opens a real encoder on a discard virtual file; sf_format_check()
// cannot answer, because it accepts MPEG whether or not the encoder was built.
// On false, `reason` (when given) says why, in a sentence for a tooltip.
//
// Declared here, in the public header, because the app has to ask and the
// writer lives in sinks/src/ where the app layer cannot include it; the app
// reaches it through tw/render's mp3ExportAvailable().
bool mp3EncoderAvailable(std::uint32_t sampleRate, std::uint32_t channels,
                         std::string *reason = nullptr);

class AudioFileWriter {
public:
    virtual ~AudioFileWriter() = default;

    virtual bool open(const std::string &path, const AudioFileConfig &config) = 0;
    virtual bool write(const float *interleaved, std::size_t frameCount) = 0;
    virtual bool close() = 0;

    virtual const char *errorMessage() const = 0;

    // The format's quality knob, as RenderParams::quality documents it: kbps
    // for MP3, 0..10 for OGG, nothing for WAV (the default no-op). Call it
    // BEFORE open(); writers read it when they configure the encoder.
    virtual void setQuality(int /*quality*/) {}
};

std::unique_ptr<AudioFileWriter> createAudioFileWriter(AudioFormat format);

}  // namespace audio

#endif
