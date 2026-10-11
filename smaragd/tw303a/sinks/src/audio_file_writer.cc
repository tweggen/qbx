#include "tw/sinks/audio_file_writer.h"

#include "ogg_writer.h"
#include "sndfile_writer.h"

namespace audio {

std::unique_ptr<AudioFileWriter> createAudioFileWriter(AudioFormat format) {
    switch (format) {
        case AudioFormat::WAV:
            return std::make_unique<SndfileWriter>(SndfileWriter::Container::WAV);
        case AudioFormat::OGG:
            return std::make_unique<OGGWriter>();
        case AudioFormat::MP3:
            return std::make_unique<SndfileWriter>(SndfileWriter::Container::MP3);
    }
    return nullptr;
}

}  // namespace audio
