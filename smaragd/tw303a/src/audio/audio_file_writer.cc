#include "audio/audio_file_writer.h"

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
            // Placeholder for MP3 writer (implemented in Phase 2c)
            return nullptr;
    }
    return nullptr;
}

}  // namespace audio
