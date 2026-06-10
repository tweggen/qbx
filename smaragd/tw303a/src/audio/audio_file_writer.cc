#include "audio/audio_file_writer.h"

#include "mp3_writer.h"
#include "ogg_writer.h"
#include "wav_writer.h"

namespace audio {

std::unique_ptr<AudioFileWriter> createAudioFileWriter(AudioFormat format) {
    switch (format) {
        case AudioFormat::WAV:
            return std::make_unique<WAVWriter>();
        case AudioFormat::OGG:
            return std::make_unique<OGGWriter>();
        case AudioFormat::MP3:
            return std::make_unique<MP3Writer>();
    }
    return nullptr;
}

}  // namespace audio
